# RE F5 Enumeration Findings — Adversarial Review

**Reviewer:** Adversarial review subagent (Ghidra MCP)
**Date:** 2026-08-20
**Document under review:** `docs/RE/RE_F5_enumeration_findings.md`
**Binary:** orlib20.dll v9.50.01.3344 (Ghidra program `/orlib20.dll`, image base `0x10000000`)

## Verdict: DISAPPROVE

The document contains **2 BLOCKING errors** (one critical correctness issue in the name
extraction strategy, one non-unique signature pattern) and **4 additional significant
errors** (wrong filter offset, inverted filter logic, wrong pattern lengths, inverted
risk assessment). The 5 original requirements are partially met, but the name-offset
risk — the most critical claim to verify — is mischaracterized and the proposed
mitigation strategy is broken.

---

## Blocking Findings

### Finding 1: Name extraction strategy for `[rec+0x73] < 6` is BROKEN — offset 0x02 is overwritten by lazy-init

**What is wrong:** The document (§4, §7) recommends reading the name from offset `0x02`
("inline short name") when `[rec+0x73] < 6`, and from offset `0x17` when
`[rec+0x73] >= 6`. This is backwards. Offset `0x02` is **overwritten** by the lazy
record-data initializer `FUN_101c5b10` and does NOT contain the name in the output
buffer of `_LoadMasterRecsEX@16`.

**Evidence from Ghidra analysis:**

`_LoadMasterRecsEX@16` calls `FUN_101c3c50()` (GetRecordData) to obtain each record's
data pointer, then `memcpy`s 0x10A bytes to the output buffer. `FUN_101c3c50` triggers
`FUN_101c5b10` (lazy-init) if the record data is not yet loaded:

```c
// FUN_101c5b10 (0x101C5B10) — key lines after SafeArray copy:
pvVar3 = operator_new__(0x10a);
*(void **)(param_1 + 0x8f) = pvVar3;
SafeArrayAccessData(psa, &local_14);
memcpy(pvVar3, local_14, 0x10a);           // copy 0x10A bytes from SafeArray
SafeArrayUnaccessData(psa);
// --- THEN OVERWRITES offsets 0x06 and 0x02: ---
*(undefined4 *)(*(int *)(param_1 + 0x8f) + 6) = *(undefined4 *)(param_1 + 0xc);
    // record[0x06..0x09] = warehouse module object pointer (NOT original data)
uVar4 = FUN_101c4990();                    // returns ProgId (dispatches cmd 0x32)
*(undefined4 *)(*(int *)(param_1 + 0x8f) + 2) = uVar4;
    // record[0x02..0x05] = ProgId as int (NOT the short name!)
```

`FUN_101c4990` (0x101C4990) dispatches command 0x32 and returns `*(short *)(param_1 + 0x15)`
— the ProgId. So after lazy-init:
- `record[0x02..0x05]` = **ProgId** (int), NOT the short name
- `record[0x06..0x09]` = **warehouse module pointer**, NOT original record data
- `record[0x17..0x36]` = long name — **preserved** (not overwritten)

This means the output buffer of `_LoadMasterRecsEX@16` has the ProgId at offset 0x02,
not the name. Reading from 0x02 for `[rec+0x73] < 6` records would compare the ProgId
against the search name — always failing to match.

**What must be fixed:** The implementation should **always read the name from offset
0x17**, regardless of `[rec+0x73]`. The branch on `[rec+0x73]` should be removed from
the name extraction strategy. Offset 0x17 is written by `FUN_10158fb0` during record
creation (`strcpy_s((char*)(rec+0x17), 0x3d, name)`) and is preserved through the
SafeArray copy + lazy-init (not overwritten).

---

### Finding 2: Command 0x09 signature pattern is NOT unique — matches 2 functions

**What is wrong:** The document (§5.2) claims `expectedMatchCount: 1` for the
`FUN_101d16c0_thread_check_INT3` signature. The proposed 95-byte pattern actually
matches **2 functions** in orlib20.dll: `FUN_101d16c0` (the intended target at
`0x101D16C0`) AND `FUN_101d0860` (a different command-0x09 dispatch helper at
`0x101D0860`).

**Evidence from Ghidra analysis:**

A `search_byte_patterns` call with the exact pattern from the document returns:
```
[{"address":"101d0860"}, {"address":"101d16c0"}]
```

`FUN_101d0860` (decompiled at `0x101D0860`) is a near-identical command-0x09 dispatch
helper. It shares the same prologue, the same `[ESI+0xAE] != 0` check, the same
`WaitForSingleObject`/`GetCurrentThreadId`/thread-check/INT3/command-0x09-store
sequence. The only differences are:
- SEH handler address: `0x101F5D62` vs `0x101F5E12` (wildcarded as `?? ?? ?? ??` in the
  pattern, so not differentiating)
- Allocation size after the pattern ends: `0x275` vs `0x241`
- Called function after the pattern ends: `FUN_1000dd30` vs `FUN_101bfb00`

Both differences occur **after** the 95-byte pattern ends, so the pattern cannot
distinguish them.

**What must be fixed:** The pattern must be extended beyond the command-byte store to
include a differentiating instruction. The most straightforward differentiator is the
allocation size: `FUN_101d16c0` pushes `0x241` (`68 41 02 00 00`) while `FUN_101d0860`
pushes `0x275` (`68 75 02 00 00`). The pattern should be extended to include this PUSH
instruction (approximately 0x27 bytes after the current pattern end). Alternatively,
the SEH handler address could be un-wildcarded, but this would make the pattern
version-specific.

---

## Significant Findings (non-blocking but must be corrected)

### Finding 3: Filter offset is 0xD8, not 0x36 — DWORD index confused with byte offset

**What is wrong:** The document (§4 record layout table, §3.1 internal flow) claims the
hidden/deleted flag is at byte offset `0x36` and the filter checks `rec[0x36] & 2`. The
actual byte offset is **0xD8**.

**Evidence from Ghidra analysis:**

The disassembly of `_LoadMasterRecsEX@16` at `0x101A551E` shows:
```asm
101a551e  TEST byte ptr [EAX + 0xd8], 0x2    ; checks rec[0xD8] bit 1, NOT rec[0x36]
```

The Ghidra decompilation shows `puVar1[0x36]` where `puVar1` is `undefined4 *` (DWORD
pointer). `puVar1[0x36]` = byte offset `0x36 × 4 = 0xD8`. The document confused the
DWORD array index (0x36) with the byte offset (0xD8).

This is confirmed by the fallback path at `0x101A53C6`:
```asm
101a53c6  AND word ptr [ECX + 0x5f], 0xfffd  ; ECX = buf+0x79, so [buf+0x79+0x5f] = [buf+0xD8]
```

And by `FUN_10158fb0`: `param_3[0x6c] & 0xfffd` where `param_3` is `short*`, so
`param_3[0x6c]` = byte offset `0x6c × 2 = 0xD8`.

**What must be fixed:** Replace all references to `rec[0x36]` with `rec[0xD8]` in the
record layout table and internal flow description.

---

### Finding 4: Filter keep/skip logic is INVERTED

**What is wrong:** The document (§3.1 internal flow) says:
```
if (rec[0x79] & 1) != 0 AND (rec[0x36] & 2) == 0:
  keep record (GlobalReAlloc to grow)
else:
  skip record
```

The actual code does the **opposite** — it SKIPS under that condition and KEEPS
otherwise.

**Evidence from Ghidra analysis:**

The disassembly at `0x101A5518`:
```asm
101a5518  TEST byte ptr [EAX + 0x79], 0x1    ; check rec[0x79] bit 0
101a551c  JZ 0x101a5527                       ; if bit 0 CLEAR → KEEP (jump to grow path)
101a551e  TEST byte ptr [EAX + 0xd8], 0x2    ; check rec[0xD8] bit 1
101a5525  JZ 0x101a556c                       ; if bit 1 CLEAR → SKIP (jump to skip path)
; fall through to 0x101a5527 → KEEP
```

At `0x101a5527` (KEEP): the buffer is grown via `GlobalReAlloc`, the write pointer
advances, and the count increments.
At `0x101a556c` (SKIP): the write pointer does NOT advance and the count does NOT
increment (next record overwrites the same slot).

So the actual logic is:
- **KEEP** when: `(rec[0x79] & 1) == 0` OR `(rec[0xD8] & 2) != 0`
- **SKIP** when: `(rec[0x79] & 1) != 0` AND `(rec[0xD8] & 2) == 0`

The document's "keep" condition is the actual "skip" condition. The semantics are also
inverted: the document calls `0x79` bit 0 the "Active flag" and says the filter
"excludes inactive" (`0x79 & 1 == 0`), but the code KEEPS records with `0x79 & 1 == 0`.
In the fallback path (unavailable warehouse), records are marked `rec[0x79] |= 1`
(hidden), and the filter correctly skips them.

**What must be fixed:** Invert the keep/skip logic in §3.1 and correct the flag
semantics in the §4 record layout table. Note: this does not affect the recommended
implementation (which uses `filterMode=0` and relies on orlib20's internal filtering),
but the incorrect documentation could mislead implementers.

---

### Finding 5: Pattern length fields are wrong for 2 of 3 signatures

**What is wrong:** The `length` field in the proposed JSON signature entries does not
match the actual number of bytes in the `hexPattern` string for commands 0x08 and 0x07.

**Evidence:**

| Command | Document says | Actual token count | Status |
|---------|--------------|-------------------|--------|
| 0x08 (`FUN_101d0c30`) | 48 | 51 | **WRONG** |
| 0x09 (`FUN_101d16c0`) | 95 | 95 | correct |
| 0x07 (`FUN_101d0d90`) | 55 | 58 | **WRONG** |

The sigtool codegen uses the `length` field to validate the pattern. A mismatch would
cause validation failures.

**What must be fixed:** Correct the `length` fields to 51 (cmd 0x08) and 58 (cmd 0x07).

---

### Finding 6: Risk assessment for name offset is INVERTED

**What is wrong:** The document (§8.1) flags the `[rec+0x73] >= 6` case (reading from
0x17) as the risk needing Frida confirmation, while presenting the `[rec+0x73] < 6`
case (reading from 0x02) as reliable. The reality is the opposite: 0x17 is the
reliable path (not overwritten by lazy-init), and 0x02 is the broken path (overwritten
with the ProgId).

**Evidence:** See Finding 1. The lazy-init `FUN_101c5b10` overwrites offset 0x02 but
preserves offset 0x17. The document's risk §8.1 asks to "verify that `rec + 0x17`
contains the correct name" for `[rec+0x73] >= 6` records, but this is the path that
IS reliable. The real risk is whether 0x17 is universally populated (including for
built-in items not created via `FUN_10158fb0`), which applies to ALL records regardless
of `[rec+0x73]`.

**What must be fixed:** Rewrite §8.1 to reflect that:
1. Offset 0x02 is unreliable (overwritten by lazy-init) — this is a static finding,
   not a Frida risk.
2. Offset 0x17 is the correct read location for ALL records.
3. The remaining Frida risk is whether 0x17 is populated for ALL records (including
   built-in items), not specifically for `[rec+0x73] >= 6`.

---

## Confirmed Claims (adequate evidence)

The following claims were independently verified via Ghidra and are CORRECT:

1. **Parameter order of `_LoadMasterRecsEX@16`**: `(category, outBuf, path, filterMode)`
   — confirmed by decompilation of the function body AND the authoritative caller
   `FUN_1015b760` which calls `_LoadMasterRecsEX_16(param_2, &local_8, local_11c, 0)`.
   ✓

2. **Export ordinal/RVA**: Ordinal 568, RVA `0x1A5360` — confirmed from Ghidra's
   decompilation header comment. ✓

3. **Type conversion `FUN_101d43a0`**: Category 2 expands to `0x502, 0x402, 0x602,
   0x802, 0xA02, 0xC02, 0xE02, 0x1002, 0x1202, 0x1402` — confirmed by decompilation.
   TradingApp (0x1402) IS included under category 2. ✓

4. **Record size 0x10A**: Confirmed by the `0x42` DWORD + 1 WORD memcpy in
   `_LoadMasterRecsEX@16` and by `FUN_101c5b10` (`operator_new__(0x10a)` +
   `memcpy(..., 0x10a)`). ✓

5. **ProgId at offset 0x00** (short): Confirmed by `FUN_10158fb0`: `*param_3 = param_5`.
   Not overwritten by lazy-init. ✓

6. **Long name at offset 0x17** (61 bytes, `strcpy_s` with 0x3D): Confirmed by
   `FUN_10158fb0`: `strcpy_s((char*)((int)param_3 + 0x17), 0x3d, param_4)`. ✓

7. **Subtype at offset 0xE9**: Confirmed by `FUN_10158fb0`:
   `*(char*)((int)param_3 + 0xe9) = (char)param_2`. TradingApp has subtype 0x14. ✓

8. **INT3 offsets**: All three confirmed by disassembly:
   - `FUN_101d0c30` (cmd 0x08): INT3 at `0x101D0C58`, offset `0x28` (40) ✓
   - `FUN_101d16c0` (cmd 0x09): INT3 at `0x101D1714`, offset `0x54` (84) ✓
   - `FUN_101d0d90` (cmd 0x07): INT3 at `0x101D0DBF`, offset `0x2F` (47) ✓

9. **Command bytes**: 0x08, 0x09, 0x07 — all confirmed by `MOV DWORD [ESI+4], <cmd>`
   in disassembly. ✓

10. **`FUN_101e5970` (GetItemCount)**: Returns `*(param_1 + 0xC)` with no command
    dispatch. ✓

11. **`FUN_101e10d0` item retrieval paths**: Cached (`[item+0x28] != 0` →
    `FUN_101d0450`) vs. uncached (`[item+0x28] == 0` → `FUN_101d0540`). ✓

12. **`FUN_101d0540` server vs. local mode**: `[warehouse+0xC2] == 1` → server
    (`FUN_101e3ac0`); `!= 1` → local (`FUN_101d16c0`, command 0x09). ✓

13. **Command 0x08 pattern uniqueness**: 1 match at `0x101D0C30`. ✓

14. **Command 0x07 pattern uniqueness**: 1 match at `0x101D0D90`. ✓

15. **`FUN_101c3660` name accessor logic**: `[rec+0x73] < 6` → returns `rec + 0x02`;
    `>= 6` → calls `FUN_101d04f0` + `FUN_101c36d0` (warehouse retrieval). ✓

16. **`FUN_101c36d0` operates on warehouse objects**: Uses offsets 0x1C, 0x1D, 0x218
    (not raw record buffer fields). Dispatches command 0x34. ✓

17. **No collision with existing signatures**: The 3 proposed signature names and
    prologues do not overlap with the 13 existing signatures in
    `signatures/v9.50.01.3344.json`. ✓

---

## Assessment of the 5 Original Requirements

| # | Requirement | Status | Notes |
|---|-------------|--------|-------|
| 1 | Count of items by type | **MET** | `_LoadMasterRecsEX@16` returns count; `FUN_101e5970` reads `[obj+0xC]`. |
| 2 | Retrieve name + ProgId by index | **PARTIALLY MET** | ProgId at 0x00 is correct. Name strategy is **BROKEN** for `[rec+0x73] < 6` (offset 0x02 is overwritten — see Finding 1). Must always read from 0x17. |
| 3 | Exact export names, ordinals, RVAs | **MET** | All confirmed from PE export table / Ghidra. |
| 4 | Parameter order via Ghidra static analysis | **MET** | Confirmed via function body + authoritative caller `FUN_1015b760`. |
| 5 | Thread-check INT3 patch technique | **PARTIALLY MET** | INT3 offsets and command numbers are correct. But command 0x09 pattern is non-unique (Finding 2) and 2 pattern lengths are wrong (Finding 5). |

---

## Summary of Required Fixes

1. **[BLOCKING]** Remove the `[rec+0x73]` branch from the name extraction strategy.
   Always read from offset 0x17. Document that offset 0x02 is overwritten by
   `FUN_101c5b10` lazy-init with the ProgId.
2. **[BLOCKING]** Extend the command 0x09 signature pattern to differentiate
   `FUN_101d16c0` from `FUN_101d0860` (e.g., include the `PUSH 0x241` allocation
   size instruction).
3. **[SIGNIFICANT]** Correct the filter offset from 0x36 to 0xD8 throughout the
   document.
4. **[SIGNIFICANT]** Invert the filter keep/skip logic in §3.1.
5. **[SIGNIFICANT]** Correct pattern length fields: 48→51 (cmd 0x08), 55→58 (cmd 0x07).
6. **[SIGNIFICANT]** Rewrite §8.1 risk to reflect that 0x17 is the reliable path and
   0x02 is the broken path. The remaining Frida risk is whether 0x17 is universally
   populated, not whether it's reliable for `[rec+0x73] >= 6` specifically.
