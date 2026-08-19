# RE F5 Enumeration Findings — Adversarial Re-Review (Post-Fix)

**Reviewer:** Adversarial re-review subagent (Ghidra MCP)
**Date:** 2026-08-20
**Document under review:** `docs/RE/RE_F5_enumeration_findings.md` (revised)
**Original review:** `docs/RE/RE_F5_enumeration_review.md` (6 findings)
**Binary:** orlib20.dll v9.50.01.3344 (Ghidra program `/orlib20.dll`, image base `0x10000000`)

## Verdict: APPROVE

All 6 findings from the original review have been correctly fixed. Each fix was
independently verified via Ghidra decompilation, disassembly, byte-pattern
search, and raw memory reads. No new issues were introduced.

---

## Per-Finding Verification

### Finding 1 (BLOCKING): Name extraction — 0x02 branch removed, 0x17 is unconditional

**FIXED.** Verified by decompiling `FUN_101c5b10` at `0x101C5B10`.

**Ghidra evidence (decompilation of `FUN_101c5b10`):**
```c
// After SafeArray copy of 0x10A bytes into pvVar3:
*(undefined4 *)(*(int *)(param_1 + 0x8f) + 6) = *(undefined4 *)(param_1 + 0xc);
    // record[0x06..0x09] = warehouse module object pointer
uVar4 = FUN_101c4990();
*(undefined4 *)(*(int *)(param_1 + 0x8f) + 2) = uVar4;
    // record[0x02..0x05] = ProgId (int), NOT the short name
```

- `FUN_101c4990` (`0x101C4990`) dispatches command 0x32 (`FUN_101cf5b0(0x32)`)
  and returns `(int)*(short *)(param_1 + 0x15)` — the ProgId. Confirmed by
  decompilation.
- `FUN_101c5b10` writes to offsets +6 and +2 of the record buffer. It does
  **NOT** touch offset 0x17. The `memcpy(pvVar3, local_14, 0x10a)` copies the
  full 0x10A bytes (including 0x17), and the subsequent overwrites only affect
  0x02 and 0x06.

**Document verification:**
- §4 (line 188): "ALWAYS read the name from offset 0x17" — no branch on
  `[rec+0x73]`. ✓
- §4 (lines 194–227): CRITICAL note explains offset 0x02 is overwritten with
  ProgId, offset 0x17 is preserved. The decompilation quote matches Ghidra
  output exactly. ✓
- §4 record layout table (line 174): offset 0x02 is labeled "ProgId (int,
  overwritten by lazy-init)". ✓
- §4 record layout table (line 176): offset 0x17 is labeled "Long name ...
  Always read the name from here." ✓
- §7 implementation (line 586): `const char* storedName = rec + 0x17;` —
  unconditional, no 0x02 branch. ✓
- No stale references to reading name from 0x02 anywhere in the document. ✓

---

### Finding 2 (BLOCKING): Cmd 0x09 pattern uniqueness — extended to 132 bytes

**FIXED.** Verified by running `search_byte_patterns` with the exact 132-byte
pattern from the document.

**Ghidra evidence:**
```
search_byte_patterns(pattern="<132-byte pattern from document>")
→ [{"address":"101d16c0"}]
```

- Returns **exactly 1 match** at `0x101D16C0` (`FUN_101d16c0`). ✓
- The pattern ends with `68 41 02 00 00` (PUSH 0x241), which differentiates
  `FUN_101d16c0` from `FUN_101d0860` (which pushes 0x275 = `68 75 02 00 00`).
  ✓
- Pattern length: 132 bytes (counted independently). ✓
- Document §5.2 (line 328): states "132 bytes". ✓
- JSON signature entry (line 392): `"length": 132`. ✓
- The uniqueness note (lines 333–341) correctly explains the differentiation
  mechanism. ✓

---

### Finding 3 (SIGNIFICANT): Filter offset corrected to 0xD8

**FIXED.** Verified by disassembling the filter logic at `0x101A5510–0x101A557F`.

**Ghidra evidence:**
```asm
101a551e  TEST byte ptr [EAX + 0xd8], 0x2    ; rec[0xD8] bit 1, NOT rec[0x36]
```

- The actual byte offset is 0xD8. ✓

**Document verification — all references use 0xD8 as the byte offset:**
- §3.1 internal flow (line 83): `(rec[0xD8] & 2) != 0` ✓
- §3.1 disassembly evidence (line 97): `TEST byte ptr [EAX + 0xd8], 0x2` ✓
- §3.1 note (lines 104–108): Correctly explains `puVar1[0x36]` (DWORD index)
  = byte offset `0x36 × 4 = 0xD8`. ✓
- §4 record layout table (line 177): `0xD8 | 1 | Hidden/deleted flag (bit 1)` ✓
- §4 (line 177): `(rec[0xD8] & 2) != 0 (byte offset 0xD8 = DWORD index 0x36 × 4)` ✓
- No stale `rec[0x36]` references as a byte offset. The only mentions of 0x36
  are in the DWORD-index-to-byte-offset conversion explanation, which is
  correct. ✓

---

### Finding 4 (SIGNIFICANT): Filter keep/skip logic corrected

**FIXED.** Verified by disassembling the filter logic at `0x101A5510–0x101A557F`.

**Ghidra evidence (full disassembly):**
```asm
101a5518  TEST byte ptr [EAX + 0x79], 0x1    ; check rec[0x79] bit 0 (hidden)
101a551c  JZ  0x101a5527                      ; bit 0 CLEAR → KEEP (jump to grow path)
101a551e  TEST byte ptr [EAX + 0xd8], 0x2    ; check rec[0xD8] bit 1
101a5525  JZ  0x101a556c                      ; bit 1 CLEAR → SKIP (jump to skip path)
; fall through to 0x101a5527 → KEEP

; KEEP path at 0x101a5527:
101a552a  ADD EBX, 0x10a                      ; advance write pointer by record size
101a5539  PUSH 0x40                           ; GlobalReAlloc flags
101a5545  CALL 0x1014f630                     ; GlobalReAlloc — grow buffer

; SKIP path at 0x101a556c:
101a556c  MOV ESI, [EBP+8]                    ; reload, does NOT advance EBX
101a556f  ...                                 ; next record overwrites same slot
```

- KEEP path (0x101a5527): advances write pointer (EBX += 0x10A), calls
  GlobalReAlloc to grow buffer. ✓
- SKIP path (0x101a556c): does NOT advance EBX, next record overwrites same
  slot. ✓

**Document verification:**
- §3.1 (lines 83–86): KEEP when `(rec[0x79] & 1) == 0` OR `(rec[0xD8] & 2) != 0`;
  SKIP when `(rec[0x79] & 1) != 0` AND `(rec[0xD8] & 2) == 0`. ✓
- §3.1 disassembly evidence (lines 94–100): Matches actual disassembly exactly. ✓
- §3.1 (lines 101–102): KEEP/SKIP logic summary matches. ✓
- §4 record layout table (line 179): `0x79` is "Hidden flag (bit 0)", filter
  KEEPS when `(rec[0x79] & 1) == 0`. ✓
- No inverted logic anywhere. ✓

---

### Finding 5 (SIGNIFICANT): Pattern lengths corrected to 51 and 58

**FIXED.** Verified by reading raw bytes from both function entry points.

**Cmd 0x08 (`FUN_101d0c30` at `0x101D0C30`):**
- `read_memory(0x101D0C30, 51)` returned 51 bytes. ✓
- Actual hex: `558bec8b450883ec0856578b3d8c951f108bf16affff7672894612ffd7ff1520961f103b46767401ccff766ec7460408000000`
- Document pattern (line 258): `55 8B EC 8B 45 08 83 EC 08 56 57 8B 3D ?? ?? ?? ?? 8B F1 6A FF FF 76 72 89 46 12 FF D7 FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 08 00 00 00`
- Wildcarded bytes: `8c 95 1f 10` (WaitForSingleObject import) and `20 96 1f 10`
  (GetCurrentThreadId import). All non-wildcarded bytes match. ✓
- Document length (line 291): `"length": 51`. ✓
- Pattern search returns exactly 1 match at `0x101D0C30`. ✓

**Cmd 0x07 (`FUN_101d0d90` at `0x101D0D90`):**
- `read_memory(0x101D0D90, 58)` returned 58 bytes. ✓
- Actual hex: `558bec8b450853565733ff33db8338ff8bf174508b006affff7672894612ff158c951f10ff1520961f103b46767401ccff766ec7460407000000`
- Document pattern (line 431): `55 8B EC 8B 45 08 53 56 57 33 FF 33 DB 83 38 FF 8B F1 74 50 8B 00 6A FF FF 76 72 89 46 12 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 07 00 00 00`
- Wildcarded bytes: `8c 95 1f 10` and `20 96 1f 10`. All non-wildcarded bytes
  match. ✓
- Document length (line 468): `"length": 58`. ✓
- Pattern search returns exactly 1 match at `0x101D0D90`. ✓

---

### Finding 6 (SIGNIFICANT): Risk assessment corrected — 0x17 is reliable, 0x02 is broken

**FIXED.** Verified by reading §8.1 of the revised document.

**Document verification (§8.1, lines 618–642):**
- Title: "Name offset — RESOLVED (no Frida confirmation needed)" ✓
- "Offset 0x17 is the RELIABLE and correct read location for ALL records
  returned by `_LoadMasterRecsEX@16`." ✓
- "Offset 0x02 is BROKEN — it is overwritten by `FUN_101c5b10` (lazy
  record-data init) with the ProgId." ✓
- "No Frida confirmation is needed for the name offset." ✓
- Does NOT flag 0x17 as risky. ✓
- The residual question (whether 0x17 is universally populated for built-in
  items) is correctly described as a minor edge case, not a blocking risk. ✓
- The `[rec+0x73]` byte is correctly described as irrelevant for raw records. ✓

---

## New Issues Check

### Internal consistency
- **No stale 0x02 name path**: The only references to offset 0x02 in the name
  context are in the CRITICAL note explaining it is overwritten with the ProgId.
  No code or recommendation reads the name from 0x02. ✓
- **No stale 0x36 byte-offset references**: 0x36 appears only in the DWORD-index
  explanation (`puVar1[0x36]` = byte offset `0x36 × 4 = 0xD8`), which is
  correct. ✓
- **No inverted logic**: Filter keep/skip logic is correct throughout (§3.1,
  §4). ✓
- **No wrong lengths**: All three signature patterns have correct lengths (51,
  132, 58) in both the prose and JSON entries. ✓

### Implementation code (§7)
- Line 586: `const char* storedName = rec + 0x17;` — reads name from 0x17
  unconditionally, with a comment explaining 0x02 is overwritten. ✓
- ProgId read from offset 0x00: `short progId = *(short*)rec;` ✓
- No branch on `[rec+0x73]` in the implementation. ✓

### Record layout table (§4)
- 0x00: ProgId (short) ✓
- 0x02: ProgId (int, overwritten by lazy-init) ✓
- 0x06: Warehouse module pointer (overwritten by lazy-init) ✓
- 0x17: Long name (61 bytes, "Always read the name from here") ✓
- 0x73: Name storage type ("Irrelevant for raw records") ✓
- 0x79: Hidden flag (bit 0) ✓
- 0xD8: Hidden/deleted flag (bit 1) ✓
- 0xE9: Category/subtype ✓
- Table is consistent with all fixes. ✓

### Signature JSON entries (§5.1, §5.2, §5.3)
- `FUN_101d0c30_thread_check_INT3`: length 51, 1 match, pattern verified. ✓
- `FUN_101d16c0_thread_check_INT3`: length 132, 1 match, pattern verified. ✓
- `FUN_101d0d90_thread_check_INT3`: length 58, 1 match, pattern verified. ✓
- All three patterns include the correct command byte (0x08, 0x09, 0x07) in
  `C7 46 04 <cmd> 00 00 00`. ✓
- All three INT3 offsets (40, 84, 47) are at the correct position within the
  pattern (verified by the `CC` byte at the expected offset). ✓

---

## Non-Blocking Observations

1. **Minor range notation (line 221)**: The CRITICAL note says
   `record[0x17..0x55]` for the long name. The actual range is 0x17 + 0x3D =
   0x54, so the inclusive range should be `0x17..0x54` (61 bytes). The `0x55`
   notation could be interpreted as an exclusive end. This is purely cosmetic
   — the size (61) and `strcpy_s` length (0x3D) are correct everywhere else.
   **Non-blocking.**

2. **§8.2 and §8.3 still reference Frida**: The cmd 0x09 necessity (§8.2) and
   warehouse mode (§8.3) risks still request Frida confirmation. These are
   unrelated to the 6 findings and were not flagged in the original review.
   They are legitimate open questions for the implementation phase.
   **Non-blocking.**

---

## Summary

| Finding | Original Issue | Fix Status | Verification Method |
|---------|---------------|------------|---------------------|
| 1 (BLOCKING) | 0x02 name branch | **FIXED** | Decompiled `FUN_101c5b10` — writes ProgId to 0x02, preserves 0x17. Document says always 0x17. |
| 2 (BLOCKING) | Non-unique 0x09 pattern | **FIXED** | 132-byte pattern search returns 1 match. Includes `68 41 02 00 00`. |
| 3 (SIGNIFICANT) | Filter offset 0x36 | **FIXED** | Disassembly shows `[EAX+0xD8]`. Document uses 0xD8 everywhere. |
| 4 (SIGNIFICANT) | Inverted filter logic | **FIXED** | Disassembly confirms KEEP/SKIP paths. Document logic matches. |
| 5 (SIGNIFICANT) | Wrong pattern lengths | **FIXED** | Read 51 bytes (cmd 0x08) and 58 bytes (cmd 0x07). Both match. |
| 6 (SIGNIFICANT) | Inverted risk assessment | **FIXED** | §8.1 marks name-offset risk RESOLVED, 0x17 reliable, no Frida needed. |

**All 6 findings correctly fixed. No new issues introduced. APPROVE.**
