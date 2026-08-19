# RE F5: Enumeration API Findings — Case-Insensitive `exists`

## 1. Feasibility Verdict

**FEASIBLE** — with caveats (see §8 Risks).

Case-insensitive `exists` can be implemented by enumerating all master records
of a given component type via `_LoadMasterRecsEX@16`, iterating the returned
0x10A-byte records, and comparing the stored name case-insensitively with
`_stricmp`. The ProgId is read directly from each record. Two new thread-check
INT3 patches are required (commands 0x08 and 0x09); a third (command 0x07) is
required only if the "include all" (unfiltered) mode is used.

---

## 2. Binary Identity

| Field | Value |
|-------|-------|
| DLL | `orlib20.dll` |
| Version | `9.50.01.3344` |
| Image base | `0x10000000` |
| SHA-256 | `318DB24A4F41BDB9FF69E2E647026B788706904CF887CBB0A953E003DB22EC18` |
| PE timestamp | `0x5C01735E` |
| Text RVA | `0x1000` |
| Text virtual size | `0x1F72FC` |

---

## 3. Recommended Enumeration API

### 3.1 `_LoadMasterRecsEX@16` (primary)

| Field | Value |
|-------|-------|
| Export name (decorated) | `_LoadMasterRecsEX@16` |
| Undecorated | `LoadMasterRecsEX` |
| Ordinal | 568 |
| RVA | `0x1A5360` |
| VA | `0x101A5360` |
| Calling convention | `__stdcall` (decorated `@16` = 4 params × 4 bytes) |
| Signature | `int __stdcall LoadMasterRecsEX(int category, HGLOBAL *outBuf, const char *path, int filterMode)` |

#### Parameters (confirmed by decompilation + authoritative caller)

| Param | Stack | Type | Meaning |
|-------|-------|------|---------|
| `category` | `[EBP+8]` | `int` | Component category: 0=Strategy, 2=Indicator, 4=Function, 1=ActivityBar, 3=PaintBar |
| `outBuf` | `[EBP+0xC]` | `HGLOBAL *` | Output: receives a `GlobalAlloc` buffer of `count × 0x10A` bytes. Caller must `GlobalFree`. |
| `path` | `[EBP+0x10]` | `const char *` | Warehouse workspace path. Passed to `FUN_101a9650` (GetWarehouse), which matches by the exact string passed to `OrlibInit` at registration. **Must be `NULL`** in this project (the warehouse is registered via `OrlibInit(NULL, 1)`); passing an explicit path fails to match the NULL-registered warehouse and returns 0 records. See §7 and §8.7. |
| `filterMode` | `[EBP+0x14]` | `int` | 0 = filtered (exclude hidden/deleted); nonzero = include all. |

#### Return value

- **>0**: Number of records in the output buffer.
- **0**: No records (or warehouse unavailable with `filterMode=0`).
- **-1**: Allocation failure (`GlobalAlloc` returned NULL).

#### Internal flow (from decompilation at `0x101A5360`)

```
1. warehouse = GetWarehouse(path)               // FUN_101a9650
2. if warehouse == 0:
     if filterMode == 0: allocate 1-byte buf, return 0
     else: allocate default-count buf from DAT_10488990, mark hidden, return count
3. TypeConvert(category, typeArray)              // FUN_101d43a0
4. if filterMode != 0:
     preAllocCount = CountByTypes(typeArray)    // FUN_101d0d90 (cmd 0x07)
     preAllocCount += 1
   else:
     preAllocCount = 1                          // minimal alloc, grows via GlobalReAlloc
5. buf = GlobalAlloc(0x40, preAllocCount * 0x10A + 1)
6. for each expandedType in typeArray (until -1 sentinel):
     a. CELPrograms_ctor()                       // FUN_101e0c90
     b. SelectType(expandedType, ..., 1)         // FUN_101d0c30 (cmd 0x08)
     c. count = GetItemCount()                   // FUN_101e5970 — reads [obj+0xC], NO command
     d. for i = 0 to count-1:
          item = GetItem(i)                      // FUN_101e10d0 — may use cmd 0x09
          recordSize = GetRecordSize(item)       // FUN_101c3c70 — reads [item+0x1B3]
          recordData = GetRecordData(item)       // FUN_101c3c50 — reads [item+0x8F]
          memcpy(dstBuf, recordData, 0x10A)      // 0x42 dwords + 1 word
          if filterMode == 0:
            if (rec[0x79] & 1) == 0 OR (rec[0xD8] & 2) != 0:
              keep record (GlobalReAlloc to grow)
            else:
              skip record
          else:
            keep all records
     e. CELPrograms_dtor()                       // FUN_101e0db0
7. return total kept count
```

> **Filter logic — Ghidra disassembly evidence** (at `0x101A5518`):
> ```asm
> 101a5518  TEST byte ptr [EAX + 0x79], 0x1    ; check rec[0x79] bit 0 (hidden flag)
> 101a551c  JZ  0x101a5527                      ; if bit 0 CLEAR (not hidden) → KEEP
> 101a551e  TEST byte ptr [EAX + 0xd8], 0x2     ; check rec[0xD8] bit 1
> 101a5525  JZ  0x101a556c                      ; if bit 1 CLEAR → SKIP
> ; fall through to 0x101a5527 → KEEP (grow buffer, advance write pointer)
> ```
> - **KEEP** when: `(rec[0x79] & 1) == 0` (not hidden) OR `(rec[0xD8] & 2) != 0`
> - **SKIP** when: `(rec[0x79] & 1) != 0` (hidden) AND `(rec[0xD8] & 2) == 0`
>
> Note: `0xD8` is the byte offset. The Ghidra decompilation shows `puVar1[0x36]`
> where `puVar1` is `undefined4 *` (DWORD pointer), so `puVar1[0x36]` = byte
> offset `0x36 × 4 = 0xD8`. The fallback path (unavailable warehouse) marks
> records hidden via `rec[0x79] |= 1` and clears `rec[0xD8]` bit 1
> (`& 0xfffd`), causing them to be skipped.

#### Authoritative caller: `FUN_1015b760` (`0x1015B760`)

This internal function builds the TSDev library tree. It demonstrates the
correct parameter order and filter mode:

```c
// From FUN_1015b760 decompilation:
iVar1 = FUN_101a0230();                    // check warehouse mode
if (iVar1 == 0) {
    // File-based fallback (no warehouse)
    iVar1 = _LoadMasterRecsEXOld_16(param_2, &local_8, param_3, 0);
} else {
    // Warehouse-based enumeration
    FUN_10009eb0(param_3, 3);              // prepare path
    iVar1 = _LoadMasterRecsEX_16(param_2, &local_8, local_11c, 0);
    //                    ^category  ^outBuf   ^path    ^filterMode=0
}
// ... iterate records, build tree ...
GlobalFree(local_8);
```

**Key evidence**: `filterMode = 0` (filtered) is the value used by the UI tree
builder. This means command 0x07 is NOT invoked in the filtered path
(`preAllocCount = 1`, grows via `GlobalReAlloc`).

---

### 3.2 Type Conversion: `FUN_101d43a0` (`0x101D43A0`)

This internal function converts a category to an array of expanded warehouse
types. It is called by `_LoadMasterRecsEX@16` and by `FindExistPos`.

```c
void FUN_101d43a0(undefined4 category, undefined4 *typeArray)
```

| Category | Expanded types (sentinel = `0xFFFFFFFF`) |
|----------|------------------------------------------|
| 0 (Strategy) | `0x200` |
| 1 (ActivityBar) | `0x1` |
| 2 (Indicator) | `0x502, 0x402, 0x602, 0x802, 0xA02, 0xC02, 0xE02, 0x1002, 0x1202, 0x1402` |
| 3 (PaintBar) | `0x3` |
| 4 (Function) | `0x4` |

**Critical finding**: Category 2 (Indicator) includes `0x1402` (TradingApp,
subtype `0x14`). This means `_LoadMasterRecsEX@16(2, ...)` enumerates both
Indicators AND TradingApps. To distinguish them, check `rec[0xE9]` (category/
subtype byte): TradingApp records have `0x14` at offset `0xE9`.

> **Note**: The project's `TS_COMPONENT_TYPES.md` documents TradingApp as
> expanded type `5122` (`0x1402`), category `2`, subtype `0x14`. The
> `FUN_101d43a0` decompilation confirms this: `param_2[10] = 0x1402` in
> case 2. TradingApp IS enumerated under category 2.

---

## 4. Master Record Layout (0x10A bytes = 266 bytes)

Confirmed from `FUN_10158fb0` (record initializer, `0x10158FB0`) and
`FUN_101c3660` (name accessor, `0x101C3660`):

| Offset | Size | Field | Evidence |
|--------|------|-------|---------|
| `0x00` | 2 | **ProgId** (`short`) | `FUN_10158fb0`: `*param_3 = param_5` (ProgId) |
| `0x02` | 4 | **ProgId** (int, overwritten by lazy-init) | `FUN_101c5b10`: `*(undefined4*)(rec+0x02) = FUN_101c4990()` — overwrites original short name with ProgId. See §4 note below. |
| `0x06` | 4 | **Warehouse module pointer** (overwritten by lazy-init) | `FUN_101c5b10`: `*(undefined4*)(rec+0x06) = *(undefined4*)(param_1+0x0c)` — overwrites original data with warehouse module object pointer. |
| `0x17` | 61 | **Long name** (NUL-terminated, inline) | `FUN_10158fb0`: `strcpy_s((char*)(rec+0x17), 0x3D, name)` — **preserved** by lazy-init (not overwritten). **Always read the name from here.** |
| `0xD8` | 1 | **Hidden/deleted flag** (bit 1) | `_LoadMasterRecsEX`: filters on `(rec[0xD8] & 2) != 0` (byte offset 0xD8 = DWORD index 0x36 × 4) |
| `0x73` | 1 | **Name storage type** | `FUN_101c3660`: if `< 6`, name is inline at offset 2; if `>= 6`, name is warehouse-stored. **Irrelevant for raw records** — see §4 note below. |
| `0x79` | 1 | **Hidden flag** (bit 0) | `_LoadMasterRecsEX`: filter KEEPS records where `(rec[0x79] & 1) == 0` (not hidden); skips records where bit 0 is set AND `rec[0xD8] & 2 == 0`. Fallback path marks records hidden via `rec[0x79] \|= 1`. |
| `0xE9` | 1 | **Category/subtype** (`byte`) | `FUN_10158fb0`: `*(char*)(rec+0xE9) = (char)param_2` |
| `0x109` | 1 | **Flags** | `FUN_10158fb0`: various flag manipulations |

### Name extraction strategy for raw records

For records obtained from `_LoadMasterRecsEX@16` (raw 0x10A-byte buffers, not
warehouse objects):

**ALWAYS read the name from offset 0x17** (inline long name, max 60 chars +
NUL). This field is written by `FUN_10158fb0` during record creation
(`strcpy_s((char*)(rec+0x17), 0x3D, name)`) and is **preserved** through the
SafeArray copy + lazy-init process. It is the correct read location for ALL
records returned by `_LoadMasterRecsEX@16`, regardless of `[rec+0x73]`.

> **CRITICAL — offset 0x02 is NOT the name in the output buffer.**
> `_LoadMasterRecsEX@16` calls `FUN_101c3c50` (GetRecordData) to obtain each
> record's data pointer, then `memcpy`s 0x10A bytes to the output buffer.
> `FUN_101c3c50` triggers `FUN_101c5b10` (lazy record-data init) if the record
> data is not yet loaded. `FUN_101c5b10` **OVERWRITES** offset 0x02 with the
> ProgId and offset 0x06 with a warehouse module pointer. The decompilation of
> `FUN_101c5b10` (`0x101C5B10`) confirms this:
>
> ```c
> // FUN_101c5b10 — after SafeArray copy of 0x10A bytes:
> pvVar3 = operator_new__(0x10a);
> *(void **)(param_1 + 0x8f) = pvVar3;
> SafeArrayAccessData(psa, &local_14);
> memcpy(pvVar3, local_14, 0x10a);           // copy 0x10A bytes from SafeArray
> SafeArrayUnaccessData(psa);
> // --- THEN OVERWRITES offsets 0x06 and 0x02: ---
> *(undefined4 *)(*(int *)(param_1 + 0x8f) + 6) = *(undefined4 *)(param_1 + 0xc);
>     // record[0x06..0x09] = warehouse module object pointer (NOT original data)
> uVar4 = FUN_101c4990();                    // dispatches cmd 0x32, returns ProgId
> *(undefined4 *)(*(int *)(param_1 + 0x8f) + 2) = uVar4;
>     // record[0x02..0x05] = ProgId as int (NOT the short name!)
> ```
>
> `FUN_101c4990` (`0x101C4990`) dispatches command 0x32 and returns
> `*(short *)(param_1 + 0x15)` — the ProgId. So after lazy-init:
> - `record[0x02..0x05]` = **ProgId** (int), NOT the short name
> - `record[0x06..0x09]` = **warehouse module pointer**, NOT original record data
> - `record[0x17..0x55]` = long name — **preserved** (not overwritten)
>
> Reading from offset 0x02 would compare the ProgId against the search name —
> always failing to match. The `[rec+0x73]` name-storage-type byte is
> **irrelevant** for raw records from `_LoadMasterRecsEX@16`; it only matters
> for `FUN_101c3660` (the internal name accessor) which operates on warehouse
> objects, not raw record buffers.

### ProgId extraction

The ProgId is a 2-byte `short` at offset `0x00`. Read directly:
```c
short progId = *(short *)(record + 0x00);
```

This matches the ProgId returned by `FindExistPos` on success.

---

## 5. Thread-Check INT3 Patches

### 5.1 Command 0x08 — Type Selection (REQUIRED)

Used by `_LoadMasterRecsEX@16` to select each expanded type for iteration.

| Field | Value |
|-------|-------|
| Helper function | `FUN_101d0c30` |
| Function VA | `0x101D0C30` |
| Function RVA | `0x1D0C30` |
| INT3 VA | `0x101D0C58` |
| INT3 offset from function entry | `0x28` (40) |
| Patch | `CC` → `90` (NOP) |
| Calling convention | `__thiscall` (ECX = warehouse object) |

**Byte pattern** (51 bytes, `??` = wildcarded):
```
55 8B EC 8B 45 08 83 EC 08 56 57 8B 3D ?? ?? ?? ?? 8B F1 6A FF FF 76 72 89 46 12 FF D7 FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 08 00 00 00
```

**Disassembly context**:
```asm
PUSH EBP
MOV EBP, ESP
MOV EAX, [EBP+8]
SUB ESP, 8
PUSH ESI
PUSH EDI
MOV EDI, [__imp__WaitForSingleObject]  ; wildcarded
MOV ESI, ECX                            ; this = warehouse
PUSH -1
PUSH [ESI+0x72]                         ; mutex
MOV [ESI+0x12], EAX                     ; store type
CALL EDI                                ; WaitForSingleObject
CALL [__imp__GetCurrentThreadId]        ; wildcarded
CMP EAX, [ESI+0x76]                     ; thread check
JE +1
INT3                                    ; ← offset 40, patch CC→90
PUSH [ESI+0x6E]                         ; event
MOV DWORD [ESI+4], 8                    ; command 0x08
```

**Proposed signature entry**:
```json
{
  "name": "FUN_101d0c30_thread_check_INT3",
  "targetType": "patch_site",
  "callingConvention": "thiscall",
  "expectedRva": "0x1D0C30",
  "hexPattern": "55 8B EC 8B 45 08 83 EC 08 56 57 8B 3D ?? ?? ?? ?? 8B F1 6A FF FF 76 72 89 46 12 FF D7 FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 08 00 00 00",
  "length": 51,
  "expectedMatchCount": 1,
  "patch": {
    "offset": 40,
    "originalBytes": "CC",
    "replacementBytes": "90",
    "originalBytesMask": "FF",
    "description": "NOP thread-check INT3 in command 0x08 (type select) dispatch helper"
  },
  "containingFunction": "FUN_101d0c30",
  "provenance": {
    "author": "RE-F5",
    "date": "2026-08-20",
    "source": "Ghidra disassembly",
    "reference": "docs/RE/RE_F5_enumeration_findings.md"
  }
}
```

---

### 5.2 Command 0x09 — Item Retrieval (REQUIRED, conditional)

Used by `FUN_101d0540` → `FUN_101e10d0` when an item is not cached in the
warehouse object. In headless/local mode (`[warehouse+0xC2] != 1`), this
command loads the item from the warehouse.

| Field | Value |
|-------|-------|
| Helper function | `FUN_101d16c0` |
| Function VA | `0x101D16C0` |
| Function RVA | `0x1D16C0` |
| INT3 VA | `0x101D1714` |
| INT3 offset from function entry | `0x54` (84) |
| Patch | `CC` → `90` (NOP) |
| Calling convention | `__thiscall` (ECX = warehouse object) |

**Byte pattern** (132 bytes, `??` = wildcarded):
```
55 8B EC 64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 56 8B F1 57 83 BE AE 00 00 00 00 0F 84 ?? ?? ?? ?? 8B 7D 10 8B 45 08 6A FF C7 07 00 00 00 00 FF 76 72 89 46 12 8B 45 0C 89 46 08 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 09 00 00 00 FF 15 ?? ?? ?? ?? 6A FF FF 76 6A FF 15 ?? ?? ?? ?? FF 76 72 FF 15 ?? ?? ?? ?? 83 7E 5E 00 74 ?? 68 41 02 00 00
```

> **Uniqueness note**: The original 95-byte pattern (ending at `C7 46 04 09 00
> 00 00`) matches TWO functions: `FUN_101d16c0` (target, `0x101D16C0`) AND
> `FUN_101d0860` (a different cmd-0x09 helper, `0x101D0860`). Both share the
> identical prologue, `[ESI+0xAE]` check, thread-check/INT3/command-0x09-store
> sequence. The pattern is extended (37 additional bytes) to include the
> differentiating allocation size: `FUN_101d16c0` pushes `0x241`
> (`68 41 02 00 00`) while `FUN_101d0860` pushes `0x275`
> (`68 75 02 00 00`). A Ghidra `search_byte_patterns` call with the 132-byte
> pattern returns exactly 1 match at `0x101D16C0`. Verified unique.

**Disassembly context**:
```asm
PUSH EBP
MOV EBP, ESP
MOV EAX, FS:[0]                  ; SEH frame
PUSH -1
PUSH 0x101F5E12                  ; SEH handler (wildcarded)
PUSH EAX
MOV FS:[0], ESP
PUSH ESI
MOV ESI, ECX                     ; this = warehouse
PUSH EDI
CMP DWORD [ESI+0xAE], 0          ; check initialized
JE  exit
MOV EDI, [EBP+0x10]             ; output ptr
MOV EAX, [EBP+8]                ; type
PUSH -1
MOV DWORD [EDI], 0
PUSH [ESI+0x72]                  ; mutex
MOV [ESI+0x12], EAX              ; store type
MOV EAX, [EBP+0xC]
MOV [ESI+8], EAX                 ; store subtype/name
CALL [__imp__WaitForSingleObject]
CALL [__imp__GetCurrentThreadId]
CMP EAX, [ESI+0x76]              ; thread check
JE +1
INT3                             ; ← offset 84, patch CC→90
PUSH [ESI+0x6E]                  ; event
MOV DWORD [ESI+4], 9             ; command 0x09
; --- extended pattern (differentiator below) ---
CALL [__imp__ResetEvent]         ; wildcarded
PUSH -1
PUSH [ESI+0x6A]
CALL [__imp__WaitForSingleObject] ; wildcarded
PUSH [ESI+0x72]
CALL [__imp__WaitForSingleObject] ; wildcarded
CMP DWORD [ESI+0x5E], 0
JE  exit
PUSH 0x241                       ; ← differentiator: 0x241 (target) vs 0x275 (FUN_101d0860)
```

**Proposed signature entry**:
```json
{
  "name": "FUN_101d16c0_thread_check_INT3",
  "targetType": "patch_site",
  "callingConvention": "thiscall",
  "expectedRva": "0x1D16C0",
  "hexPattern": "55 8B EC 64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 56 8B F1 57 83 BE AE 00 00 00 00 0F 84 ?? ?? ?? ?? 8B 7D 10 8B 45 08 6A FF C7 07 00 00 00 00 FF 76 72 89 46 12 8B 45 0C 89 46 08 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 09 00 00 00 FF 15 ?? ?? ?? ?? 6A FF FF 76 6A FF 15 ?? ?? ?? ?? FF 76 72 FF 15 ?? ?? ?? ?? 83 7E 5E 00 74 ?? 68 41 02 00 00",
  "length": 132,
  "expectedMatchCount": 1,
  "patch": {
    "offset": 84,
    "originalBytes": "CC",
    "replacementBytes": "90",
    "originalBytesMask": "FF",
    "description": "NOP thread-check INT3 in command 0x09 (item retrieval) dispatch helper"
  },
  "containingFunction": "FUN_101d16c0",
  "provenance": {
    "author": "RE-F5",
    "date": "2026-08-20",
    "source": "Ghidra disassembly",
    "reference": "docs/RE/RE_F5_enumeration_findings.md"
  }
}
```

---

### 5.3 Command 0x07 — Count by Types (REQUIRED only for `filterMode != 0`)

Used by `_LoadMasterRecsEX@16` only when `filterMode != 0` (include-all mode)
to pre-allocate the output buffer. **NOT needed when `filterMode = 0`**
(the recommended filtered mode, matching the authoritative caller).

| Field | Value |
|-------|-------|
| Helper function | `FUN_101d0d90` |
| Function VA | `0x101D0D90` |
| Function RVA | `0x1D0D90` |
| INT3 VA | `0x101D0DBF` |
| INT3 offset from function entry | `0x2F` (47) |
| Patch | `CC` → `90` (NOP) |
| Calling convention | `__thiscall` (ECX = warehouse object) |

**Byte pattern** (58 bytes, `??` = wildcarded):
```
55 8B EC 8B 45 08 53 56 57 33 FF 33 DB 83 38 FF 8B F1 74 50 8B 00 6A FF FF 76 72 89 46 12 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 07 00 00 00
```

**Disassembly context**:
```asm
PUSH EBP
MOV EBP, ESP
MOV EAX, [EBP+8]                 ; type array
PUSH EBX
PUSH ESI
PUSH EDI
XOR EDI, EDI                     ; count = 0
XOR EBX, EBX                     ; index = 0
CMP DWORD [EAX], -1              ; sentinel check
MOV ESI, ECX                     ; this = warehouse
JE  done
MOV EAX, [EAX]                   ; first type
PUSH -1
PUSH [ESI+0x72]                  ; mutex
MOV [ESI+0x12], EAX              ; store type
CALL [__imp__WaitForSingleObject]
CALL [__imp__GetCurrentThreadId]
CMP EAX, [ESI+0x76]              ; thread check
JE +1
INT3                             ; ← offset 47, patch CC→90
PUSH [ESI+0x6E]                  ; event
MOV DWORD [ESI+4], 7             ; command 0x07
```

**Proposed signature entry**:
```json
{
  "name": "FUN_101d0d90_thread_check_INT3",
  "targetType": "patch_site",
  "callingConvention": "thiscall",
  "expectedRva": "0x1D0D90",
  "hexPattern": "55 8B EC 8B 45 08 53 56 57 33 FF 33 DB 83 38 FF 8B F1 74 50 8B 00 6A FF FF 76 72 89 46 12 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 07 00 00 00",
  "length": 58,
  "expectedMatchCount": 1,
  "patch": {
    "offset": 47,
    "originalBytes": "CC",
    "replacementBytes": "90",
    "originalBytesMask": "FF",
    "description": "NOP thread-check INT3 in command 0x07 (count by types) dispatch helper"
  },
  "containingFunction": "FUN_101d0d90",
  "provenance": {
    "author": "RE-F5",
    "date": "2026-08-20",
    "source": "Ghidra disassembly",
    "reference": "docs/RE/RE_F5_enumeration_findings.md"
  }
}
```

---

### 5.4 Already-patched commands (NO new patches needed)

| Command | Helper | Existing signature | Used in enumeration? |
|---------|--------|--------------------|---------------------|
| `0x35` | `FUN_101cf5b0` | `FUN_101cf5b0_thread_check_INT3` (offset 36) | Yes — lazy-init of record data (`FUN_101c5b10`) |
| `0x0F` | `FUN_101d0950` | `FUN_101d0950_thread_check_INT3` (offset 67) | No (load program) |
| `0x11` | `FUN_101d0e80` | `FUN_101d0e80_thread_check_INT3` (offset 57) | No (source-text load) |
| `0x17` | `FUN_101d1fa0` | `FUN_101d1fa0_thread_check_INT3` (offset 40) | No (save/commit) |
| `0x1E` | `FUN_101cf5b0` | `FUN_101cf5b0_thread_check_INT3` (offset 36) | No (ProgId retrieval) |

Command `0x35` is used by `FUN_101c5b10` (lazy record-data initialization),
which is called from `FUN_101c3c70`/`FUN_101c3c50` (record size/data
accessors). These are called during the record-copy loop in
`_LoadMasterRecsEX@16`. The existing `FUN_101cf5b0` patch covers this.

---

## 6. Other Enumeration APIs (reference)

All ordinals and RVAs confirmed from the PE export table:

| Export name | Ordinal | RVA | Parameters | Role |
|-------------|---------|-----|------------|------|
| `_LoadMasterRecsEX@16` | 568 | `0x1A5360` | `(cat, &buf, path, filter)` | **Recommended** — bulk load all records |
| `_GetNumofMasterRecsEX@4` | 521 | `0x1A0B80` | `(typeArray)` | Count only; uses cmd 0x07; void return (side-effect) |
| `_GetMasterRecsPtr@8` | 514 | `0x1A0BC0` | `(cat, outBuf)` | Bulk via cmd 0x03; copies to caller buffer |
| `_GetMasterRecsBuf@12` | 512 | `0x1A0C70` | `(cat, &count, ?)` | Bulk via cmd 0x1A; returns buffer pointer |
| `_GetMasterRecsEX@8` | 513 | `0x107B30` | `(cat, ?)` | Alternative bulk API |
| `_GetMasterRecByType@16` | 510 | `0x1A0460` | `(cat, subType, idx, &outRec)` | Single record by type+index |
| `_GetMasterRecByName@12` | 509 | `0x1A0500` | `(cat, name, &outRec)` | Single record by name (uses cmd 0x09) |
| `_LoadMasterRecsEXOld@16` | 569 | `0x11F910` | `(cat, &buf, path, filter)` | File-based fallback (reads .MST files) |
| `_GetMasterRecOld@16` | 511 | `0x11EDF0` | — | Old single-record API |
| `_GetMaxNumOfMasters@8` | 515 | `0x11F4D0` | — | Max master count |
| `_DelMasterRecsBuf@4` | 395 | `0x1A0CC0` | `(buf)` | Free buffer |
| `FindExistPos` | 453 | `0x1A4230` | `(name, &type)` | Exact-match lookup; returns ProgId or -1 |
| `_FindExistPosSubType@12` | 454 | `0x1A41B0` | — | Exact-match with subtype |
| `_CreateEntryEX@24` | 389 | `0x159610` | — | Create new record |

### Why `_LoadMasterRecsEX@16` is recommended over alternatives

- **`_LoadMasterRecsEXOld@16`**: Reads `.MST` files from disk. However, no
  `.MST` files exist on this machine (`MyWork\Bin` contains only `.ell`/`.xml`
  files). The warehouse stores master records in memory, so the file-based
  fallback returns empty results. **Not viable.**

- **`_GetMasterRecsPtr@8`**: Uses command 0x03 (needs new patch). Simpler
  parameter list (no path), but less control over filtering. Does not
  distinguish between filtered and unfiltered modes.

- **`_GetMasterRecsBuf@12`**: Uses command 0x1A (needs new patch). Returns a
  buffer pointer and count. Similar to `_LoadMasterRecsPtr@8` but with a
  different command.

- **`_GetMasterRecByType@16`**: Retrieves a single record by type+index. Would
  require a count API to know the range, and multiple calls (one per record).
  Less efficient than bulk loading.

- **`_GetMasterRecByName@12`**: Retrieves a single record by name. Uses
  command 0x09 for warehouse-side name matching (exact match only, not
  case-insensitive). Not suitable for case-insensitive search.

`_LoadMasterRecsEX@16` is the best choice because:
1. It bulk-loads all records in one call.
2. It handles type expansion internally (including TradingApp under category 2).
3. It has a proven authoritative caller (`FUN_1015b760`).
4. With `filterMode=0`, it only requires command 0x08 (plus 0x09 for uncached
   items, plus already-patched 0x35 for lazy-init).

---

## 7. Recommended Implementation

### Minimal call sequence for case-insensitive `exists`

```cpp
// 1. Apply patches (before calling any warehouse API):
//    - FUN_101d0c30: NOP INT3 at offset 0x28 (command 0x08) — REQUIRED
//    - FUN_101d16c0: NOP INT3 at offset 0x54 (command 0x09) — REQUIRED
//    - FUN_101cf5b0: already patched (command 0x35) — NO ACTION
//    - FUN_101d0d90: NOP INT3 at offset 0x2F (command 0x07) — ONLY if filterMode != 0

// 2. Load all records for the category:
//    IMPORTANT: the path argument MUST be NULL (not getMyWorkBinPath()).
//    The orlib20 warehouse is initialized with OrlibInit(NULL, 1), which
//    registers the default warehouse under a NULL path. GetWarehouse(path)
//    matches by the exact path string passed at registration, so an explicit
//    absolute path cannot match the NULL-registered warehouse and yields
//    0 records. Confirmed by live testing: NULL -> 1476 records,
//    explicit path -> 0 records.
HGLOBAL buf = NULL;
int count = LoadMasterRecsEX(category, &buf, (const char*)NULL, 0 /*filtered*/);
if (count <= 0) {
    // item does not exist
    return { exists: false };
}

// 3. Iterate records and compare names case-insensitively:
char* records = (char*)GlobalLock(buf);
for (int i = 0; i < count; i++) {
    char* rec = records + i * 0x10A;

    // Extract name (ALWAYS from offset 0x17 — see §4):
    // NOTE: offset 0x02 is overwritten with the ProgId by FUN_101c5b10
    // (lazy-init), so it does NOT contain the name in the output buffer.
    const char* storedName = rec + 0x17;   // inline long name (max 60 chars + NUL)

    // Case-insensitive comparison:
    if (_stricmp(storedName, searchName) == 0) {
        short progId = *(short*)rec;  // offset 0x00
        GlobalUnlock(buf);
        GlobalFree(buf);
        return { exists: true, name: storedName, progId: progId };
    }
}

GlobalUnlock(buf);
GlobalFree(buf);
return { exists: false };
```

### TradingApp handling

Since category 2 (Indicator) includes TradingApp (expanded type `0x1402`),
enumerating category 2 returns both Indicators and TradingApps. To search
only TradingApps, filter by `rec[0xE9] == 0x14` (subtype byte). To search
only Indicators, filter by `rec[0xE9] != 0x14`.

For the CLI's `--type tradingapp` option, the implementation should:
1. Call `_LoadMasterRecsEX@16(2, ...)` (category 2).
2. Filter records where `rec[0xE9] == 0x14` (TradingApp subtype).
3. Compare names only against TradingApp records.

---

## 8. Risks and Unknowns

### 8.1 Name offset — RESOLVED (no Frida confirmation needed)

**Offset 0x17 is the RELIABLE and correct read location for ALL records**
returned by `_LoadMasterRecsEX@16`. This is a static finding confirmed by
Ghidra decompilation — no Frida confirmation is needed for the name offset.

**Offset 0x02 is BROKEN** — it is overwritten by `FUN_101c5b10` (lazy
record-data init) with the ProgId (via `FUN_101c4990`, which dispatches
command 0x32). The decompilation of `FUN_101c5b10` at `0x101C5B10` confirms
this (see §4 for the full quote). Reading from offset 0x02 would compare the
ProgId against the search name — always failing to match.

The `[rec+0x73]` name-storage-type byte is **irrelevant** for raw records from
`_LoadMasterRecsEX@16`. It only matters for `FUN_101c3660` (the internal name
accessor), which operates on warehouse objects (with state at offsets 0x1C,
0x1D, 0x218), NOT on raw record buffers. For raw records, offset 0x17 is
always the correct location — it is written by `FUN_10158fb0` during record
creation (`strcpy_s((char*)(rec+0x17), 0x3D, name)`) and preserved through the
SafeArray copy + lazy-init (not overwritten).

**No Frida confirmation is needed for the name offset.** The only residual
question is whether offset 0x17 is universally populated for ALL records
(including built-in items not created via `FUN_10158fb0`), but this applies
equally to all records regardless of `[rec+0x73]` and is a minor edge case
that does not affect the recommended implementation.

### 8.2 Command 0x09 necessity (NEEDS FRIDA CONFIRMATION)

`FUN_101e10d0` (item retrieval) has two paths:
- If `[item+0x28] != 0`: calls `FUN_101d0450` (no warehouse command — uses
  cached object).
- If `[item+0x28] == 0`: calls `FUN_101d0540`, which in local mode
  (`[warehouse+0xC2] != 1`) calls `FUN_101d16c0` (command 0x09).

Whether command 0x09 is actually triggered depends on whether items are
cached during enumeration. In a fresh headless session, items may not be
cached, requiring command 0x09. **Frida confirmation needed:**
- Trace `_LoadMasterRecsEX@16` and log whether `FUN_101d16c0` is called.
- If it is, confirm the INT3 patch at offset 0x54 allows it to proceed.

### 8.3 Warehouse mode (server vs. local)

`FUN_101d0540` checks `[warehouse+0xC2]`:
- `== 1` (server mode): calls `FUN_101e3ac0` (no command 0x09).
- `!= 1` (local mode): calls `FUN_101d16c0` (command 0x09).

In the project's headless scenario, the warehouse is likely in local mode
(since there's no TSDev.exe UI acting as a server). But this has not been
confirmed. If server mode is active, command 0x09 patch is unnecessary.

### 8.4 Empty warehouse / no .MST files

No `.MST` files exist on disk at `MyWork\Bin`. The warehouse manages master
records in memory. If the warehouse is not properly initialized (via
`_OrlibInit@8`), `_LoadMasterRecsEX@16` will return 0 (filtered mode) or
allocate a default-count buffer of hidden records (unfiltered mode). The
implementation must handle the empty case gracefully.

### 8.5 Record count from `DAT_10488990`

When the warehouse is unavailable and `filterMode != 0`,
`_LoadMasterRecsEX@16` reads a default count from `(&DAT_10488990)[category]`.
A Ghidra memory read at `0x10488990` showed five values of `1000` (for
categories 0-4). This is an internal maximum/default table, not the actual
record count. The records in this fallback path are marked as hidden
(`rec[0x79] |= 1`) and should be skipped by the case-insensitive search.

### 8.6 `FUN_101d0d90` return value (void vs. int)

Ghidra inferred a `void` return for `_GetNumofMasterRecsEX@4` because the
function calls `FUN_101d0d90` and discards its result. The actual count is
accumulated as a side effect within the warehouse object. When
`_LoadMasterRecsEX@16` calls `FUN_101d0d90` directly (in unfiltered mode), it
uses the return value as a pre-allocation count. This suggests the return
value IS the count, but the public `_GetNumofMasterRecsEX@4` export discards
it. This is not relevant to the recommended implementation (which uses
`filterMode=0` and does not call `FUN_101d0d90`).

### 8.7 Path argument — RESOLVED (must be NULL, not getMyWorkBinPath())

**RESOLVED.** The `path` parameter to `_LoadMasterRecsEX@16` must be `NULL`,
not an explicit absolute path such as `getMyWorkBinPath()`. The orlib20
warehouse is initialized in this project with `OrlibInit(NULL, 1)`, which
registers the default warehouse under a `NULL` path string. The internal
`GetWarehouse(path)` (`FUN_101a9650`) matches by the exact path string passed
at registration, so an explicit absolute path cannot match the NULL-registered
warehouse and the call returns 0 records.

This was confirmed by live testing during implementation of
`enumerateRecords`:
- `path = NULL`  → 1476 records found (enumeration works).
- `path = getMyWorkBinPath()` (explicit absolute path) → 0 records found.

The earlier RE recommendation to pass `getMyWorkBinPath()` (based on the
authoritative caller `FUN_1015b760`, which passes a path derived from
`param_3`) was **incorrect for this project's initialization**: that caller
operates inside TSDev.exe, where the warehouse is registered with an explicit
path. In the headless CLI, `OrlibInit(NULL, 1)` is used, so `NULL` is the
matching key. All other orlib20 functions in this project access the default
warehouse via the NULL-registered handle, consistent with passing `NULL`
here.

---

## 9. Summary of Required Changes (for implementation, NOT this RE doc)

| Item | Action | Details |
|------|--------|---------|
| Signature: `FUN_101d0c30_thread_check_INT3` | **ADD** to `signatures/v9.50.01.3344.json` | Command 0x08, offset 40, `CC→90` |
| Signature: `FUN_101d16c0_thread_check_INT3` | **ADD** to `signatures/v9.50.01.3344.json` | Command 0x09, offset 84, `CC→90` |
| Signature: `FUN_101d0d90_thread_check_INT3` | **ADD** (only if `filterMode != 0`) | Command 0x07, offset 47, `CC→90` |
| Export: `_LoadMasterRecsEX@16` | **ADD** to exports section | Ordinal 568, RVA `0x1A5360`, stdcall |
| Resolver | Add resolution for `_LoadMasterRecsEX@16` | Via export name lookup |
| TsDevLib | Add `enumerateRecords()` method | Calls `_LoadMasterRecsEX@16`, iterates 0x10A records |
| CLI `exists --case-insensitive` | Add case-insensitive flag | Uses enumeration + `_stricmp` |
| Python wrapper | Add `case_insensitive` param to `exists()` | Drives CLI flag |

> **This document is research-only.** No production source, signature files,
> or tools were modified during this investigation.
