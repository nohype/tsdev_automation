# RE Findings: `_LoadTextFileEX@16` (orlib20.dll v9.50.01.3344)

**Date:** 2026-08-19
**Analyst:** RE subagent (Ghidra MCP)
**Binary:** `orlib20.dll` (image base `0x10000000`, file version 9.50.01.3344, SHA-256 `318DB24A...`)
**Scope:** Confirm parameter order, return-value semantics, and calling convention of the named export `_LoadTextFileEX@16` (the source-text retrieval primitive, counterpart to `_SaveTextEX@16`).

> **CORRECTION — 2026-08-23:** The ABI/parameter-order findings remain useful, but this document's interpretation of argument 4 and "password-protected items" is superseded. Library items do not expose user password protection. Some source is internally encrypted on disk (master-record flag `0x400`). TSDev passes a non-NULL zeroed 64-byte metadata buffer for those items, which triggers decryption and is populated from the data header; NULL takes the raw-copy path. Production `getSource()` now follows this behavior and returns plaintext. Treat every later "password-protected" statement in this file as historical terminology, not current guidance. See `docs/RE/RE_M2_SAVEMODULE_NEW.md` §6 and `docs/FEASIBILITY.md` §5.1 corrected note.

**Verdict:** Parameter order, return-handle ownership, and calling convention are confirmed; password semantics are corrected by the banner above.

---

## 1. Addresses & call graph

| Symbol | Address (VA) | RVA | Role |
|--------|-------------|-----|------|
| `_LoadTextFileEX@16` | `0x101081A0` | `0x1081A0` | Exported thunk (10 instructions, `RET 0x10`) |
| `FUN_101a0f90` | `0x101A0F90` | `0x1A0F90` | Real implementation (callee of the thunk, `RET 0x14`) |
| `_SaveTextEX@16` | `0x10108160` | `0x108160` | Exported thunk (the save counterpart) |
| `FUN_101a08b0` | `0x101A08B0` | `0x1A08B0` | Real save implementation (callee of SaveTextEX thunk) |
| `FUN_10108630` | `0x10108630` | `0x108630` | **Internal caller** of both `FUN_101a0f90` and `_SaveTextEX@16` — the authoritative mirror evidence |
| `FUN_101d43a0` | `0x101D43A0` | `0x1D43A0` | TypeConvert (WHType → subtype list) |
| `FUN_101cff60` | `0x101CFF60` | `0x1CFF60` | Module lookup by type-buffer + progId |
| `FUN_101d0e80` | `0x101D0E80` | `0x1D0E80` | Warehouse command-0x11 dispatcher (contains the thread-check INT3) |

**Cross-references to `_LoadTextFileEX@16` (`0x101081A0`):**
- `0x1021D878` (DATA — export table slot)
- Entry Point (EXTERNAL — it is a PE export)

There are **no internal call-site xrefs** to the `_LoadTextFileEX@16` thunk from within orlib20.dll itself; the only internal caller (`FUN_10108630`) bypasses the thunk and calls the real implementation `FUN_101a0f90` directly. The thunk exists purely for external callers (TSDev.exe / ELIS.exe). This is still strong evidence: `FUN_10108630` passes arguments to `FUN_101a0f90` in the exact positions the thunk forwards, and it calls `_SaveTextEX@16` with the mirror layout in the same function.

---

## 2. Question 1 — Parameter order (CONFIRMED)

### 2.1 Thunk disassembly (`_LoadTextFileEX@16` @ `0x101081A0`)

```asm
101081a0  PUSH EBP
101081a1  MOV EBP,ESP
101081a3  PUSH dword ptr [EBP + 0x14]   ; L4 (4th caller arg)  -> FUN P5
101081a6  PUSH dword ptr [EBP + 0x10]   ; L3 (3rd caller arg)  -> FUN P4
101081a9  PUSH dword ptr [EBP + 0xc]    ; L2 (2nd caller arg)  -> FUN P3
101081ac  PUSH dword ptr [EBP + 0x8]    ; L1 (1st caller arg)  -> FUN P2
101081af  PUSH dword ptr [0x1023aab8]   ; DAT_1023aab8 (hinst) -> FUN P1
101081b5  CALL 0x101a0f90               ; FUN_101a0f90 (stdcall, RET 0x14)
101081ba  POP EBP
101081bb  RET 0x10                      ; callee cleans 16 bytes (4 args x 4)
```

The thunk prepends the global `DAT_1023aab8` (the `errorHinst` HINSTANCE — RVA `0x23AAB8`, already catalogued in `signatures/v9.50.01.3344.json` `dataGlobals[2]`) as the first argument to `FUN_101a0f90`, then forwards the 4 caller arguments unchanged. So `FUN_101a0f90(P1=hinst, P2=L1, P3=L2, P4=L3, P5=L4)`.

### 2.2 Real implementation (`FUN_101a0f90` @ `0x101A0F90`) — decompiled

```c
HGLOBAL FUN_101a0f90(undefined4 hinst,        // P1 = DAT_1023aab8 (internal)
                     undefined4 p2_unused,    // P2 = L1 (caller param_1)
                     undefined4 p3_whType,    // P3 = L2 (caller param_2)
                     undefined4 p4_progId,    // P4 = L3 (caller param_3)
                     void *p5_password)       // P5 = L4 (caller param_4)
{
  int wh = GetWarehouse(0);                   // FUN_101a9650(0)
  if (wh != 0) {
    FUN_101d43a0(p3_whType, local_54);        // TypeConvert(whType -> subtype list)
    rec = FUN_101cff60(local_54, p4_progId, 0); // module lookup by type+progId
    if (rec != NULL) {
      local_14 = FUN_101d4280(p3_whType, rec[0xe9]);
      src = FUN_101d0e80(local_14, p4_progId, &len); // command 0x11 dispatch
      hglob = GlobalAlloc(0x40, len + 10);
      if (hglob != NULL) {
        if (p5_password == NULL) {
          memcpy(hglob, src, len);            // plain copy for standard items
        } else {
          ...                                 // password decrypt path
        }
        ...
        return hglob;
      }
    }
  }
  return NULL;
}
```

**How each parameter is used (evidence):**

| FUN param | Caller (`_LoadTextFileEX`) param | Stack offset in FUN | Usage in body | Meaning |
|-----------|----------------------------------|---------------------|---------------|---------|
| P1 (`hinst`) | *(internal — `DAT_1023aab8`)* | `[EBP+0x8]` | Re-read from `0x1023aab8` directly at error-report sites (`0x101A1017`, `0x101A119C`); passed to `FUN_10154780` (LoadString error helper). Not a caller argument. | HINSTANCE for error strings |
| P2 | **param_1** (L1) | `[EBP+0xc]` | Only read at `0x101A1014`/`0x101A1199` and forwarded to the error-reporter `FUN_10154780`. **Not used in the load/lookup/decrypt logic.** | **unused** (pass NULL) |
| P3 | **param_2** (L2) | `[EBP+0x10]` | `PUSH [EBP+0x10]` before `CALL FUN_101d43a0` (TypeConvert) at `0x101A0FB1`; again before `FUN_101d4280` at `0x101A0FD8`. `FUN_101d43a0` is a `switch(param)` on `0..4` producing the WHType subtype list. | **whType** (warehouse type code) |
| P4 | **param_3** (L3) | `[EBP+0x14]` | `MOV EBX,[EBP+0x14]` at `0x101A0FB9`; EBX passed to `FUN_101cff60` (module lookup) and `FUN_101d0e80` (command-0x11 dispatch, stored at `obj+0x12`). | **progId** |
| P5 | **param_4** (L4) | `[EBP+0x18]` | `MOV ECX,[EBP+0x18]; TEST ECX,ECX; JZ plain_copy` at `0x101A1097`. NULL → `memcpy` plain text; non-NULL → password-decryption path. | **password buffer** (NULL for standard items) |

### 2.3 TypeConvert confirms P3 = whType (`FUN_101d43a0`)

```c
void FUN_101d43a0(int whType, int* out) {
  switch(whType) {
    case 0: out[0]=0;  out[1]=0x200; out[2]=-1; return;   // Strategy
    case 1: out[0]=1;  out[1]=-1;    return;              // (Function — static survey)
    case 2: out[0]=2;  out[1]=0x502; ... out[0xb]=-1; ... // Indicator + subtypes
    case 3: out[0]=3;  out[1]=-1;    return;
    case 4: out[0]=4;  out[1]=-1;    return;              // Function (confirmed)
    default: out[0]=-1; return;
  }
}
```

This is the same TypeConvert used by `FindExistPos`, `_CreateEntryEX`, `_SaveModule_New`, `_VerifyModule_New`, and `_SaveTextEX` — the canonical WHType expander (FEASIBILITY §5.2/§5.5). **P3 is unambiguously whType.**

### 2.4 Authoritative caller evidence (`FUN_10108630` @ `0x10108630`)

This internal function calls **both** the load impl and the save export, making it the strongest possible mirror evidence:

```c
void __fastcall FUN_10108630(param_1, param_2, param_3_progId, param_4, param_5, param_6_rec)
{
  ...
  if ((param_2 == 0) && ((param_6[0x74] & 1) == 0)) {
    if (param_4 == 2 || param_4 == 0) {
      // --- LOAD path: calls FUN_101a0f90 (= _LoadTextFileEX impl) ---
      if (param_5 != 0) { pw = local_64; memset(pw, 0, 0x40); }
      hMem = (HGLOBAL)FUN_101a0f90(DAT_1023aab8, local_8, 0 /*whType*/,
                                   param_3 /*progId*/, pw /*password*/);
      ...
      GlobalFree(hMem);          // confirms HGLOBAL + GlobalFree semantics
    } else if (param_4 == 1) {
      // --- SAVE path: calls _SaveTextEX@16 directly ---
      hMem = (HGLOBAL)FUN_101087d0(param_1, 0, param_3, 1, 0);
      _SaveTextEX_16(hMem /*sourceText*/, 0 /*whType*/,
                     param_3 /*progId*/, 0 /*password=NULL*/);
      ...
    }
  }
}
```

The load call passes `(hinst, local_8, 0=whType, param_3=progId, pw=password)` — i.e. caller param_1 (`local_8`) lands in the **unused** P2 slot, `0` lands in the **whType** P3 slot, `param_3` lands in the **progId** P4 slot, and `pw` lands in the **password** P5 slot. This independently confirms the order derived from the thunk + body analysis.

### 2.5 Confirmed parameter table

| param index | name | C++ type | meaning | evidence |
|------------|------|----------|---------|----------|
| 1 | `unused` | `void*` | Unused in the load path; only forwarded to the error-report helper. Pass `NULL`. | Thunk forwards to FUN P2 (`[EBP+0xc]`); body uses P2 only at error sites; caller `FUN_10108630` passes `local_8` here with no effect on the result. |
| 2 | `whType` | `int` | Warehouse type code (0=Strategy, 2=Indicator, 4=Function, 5122=TradingApp). | FUN P3 → `FUN_101d43a0` TypeConvert switch; caller passes `0`. |
| 3 | `progId` | `int` | The item's ProgId (positive integer from `FindExistPos`). | FUN P4 → `FUN_101cff60` module lookup + `FUN_101d0e80` command-0x11 dispatch (stored at `obj+0x12`); caller passes `param_3`. |
| 4 | `metadataBuffer` | `void*` | Corrected semantics: `NULL` selects raw copy for flag-clear storage; a writable non-NULL zeroed 64-byte buffer selects TSDev's internal decryption/metadata path for storage flagged `0x400`. This is not user password protection. | FUN P5 → `TEST ECX,ECX; JZ plain_copy` at `0x101A1097`; later dynamic/static correction established TSDev's zeroed-buffer use. |

**Corrected summary:** `param_1 = unused/NULL`, `param_2 = whType`, `param_3 = progId`, and `param_4 = optional writable metadata/decryption buffer`; select the buffer path from warehouse encryption metadata as described above.

---

## 3. Question 2 — Return value semantics (CONFIRMED)

### 3.1 Return type is `HGLOBAL`

`FUN_101a0f90` is declared `HGLOBAL` by Ghidra. The success path returns `local_c` (the result of `GlobalAlloc`); the failure path executes `XOR EAX,EAX` (`0x101A103F`) and returns `NULL`.

```c
_Dst = GlobalAlloc(0x40, local_8 + 10);   // 0x101A0FFF-0x101A1002
local_c = _Dst;
if (_Dst != NULL) { ...; return local_c; } // 0x101A11E5 (success)
...
return (HGLOBAL)0x0;                        // 0x101A103F (failure)
```

### 3.2 `GlobalAlloc` flag = `0x40` (`GMEM_ZEROINIT`, fixed)

The instruction at `0x101A1000` is `PUSH 0x40` followed by `CALL [0x101F965C]` (the `GlobalAlloc` IAT entry). `0x40` = `GMEM_ZEROINIT` (without `GMEM_MOVEABLE` `0x0002`), so the allocation is **fixed + zero-initialized**. For a fixed allocation the `HGLOBAL` value *is* the pointer; `GlobalLock` returns the same pointer (effectively a no-op) and `GlobalUnlock` returns `TRUE`. The implementation writes into the handle directly via `memcpy(_Dst, _Src, ...)` which confirms the handle is directly usable as a pointer.

**Practical guidance:** Callers should still use the standard `GlobalLock` / `GlobalUnlock` / `GlobalFree` sequence — it is correct for both fixed and moveable handles and costs nothing. The returned buffer contains the source text (ANSI, length `local_8`, with a small header/terminator region of ~9–10 bytes allocated but the `memcpy` copies exactly `local_8` bytes of source).

### 3.3 Caller confirms `GlobalFree` release

In `FUN_10108630`:
```c
hMem = (HGLOBAL)FUN_101a0f90(...);
...
GlobalFree(hMem);          // release
```
The caller treats the return as an `HGLOBAL` and releases it with `GlobalFree`. No `GlobalLock` appears in this particular caller because it replaces the buffer with a freshly-allocated one rather than reading it, but the implementation's own `memcpy(_Dst, ...)` proves the handle is directly dereferenceable (and `GlobalLock` is the portable way to obtain that pointer).

### 3.4 NULL = failure

All failure paths (warehouse not initialized, module not found, `GlobalAlloc` failure, or malformed/too-short internally encrypted data) converge on `return (HGLOBAL)0x0`. **A `NULL` return unambiguously means failure.**

---

## 4. Question 3 — Calling convention & decoration (CONFIRMED)

- **Decoration:** `_LoadTextFileEX@16` — the `@16` suffix = 16 bytes of arguments = **4 parameters × 4 bytes**, the standard MSVC stdcall decoration.
- **Convention:** `__stdcall` (callee-cleaned). The thunk ends with `RET 0x10` (cleans 16 bytes / 4 args). The real implementation `FUN_101a0f90` ends with `RET 0x14` (cleans 20 bytes / 5 args — the 5th being the internal `hinst` the thunk prepends).
- **Callee-saved registers:** `FUN_101a0f90` preserves `EBX`, `ESI`, `EDI` (prologue `PUSH EBX; PUSH ESI; PUSH EDI` at `0x101A0F96-0x101A0F98`; epilogue restores them before `RET 0x14`). It is therefore safe to call from C++ via a function pointer with no special register-preservation obligations on the caller.
- **Stack implication for C++:** Because it is `__stdcall` with 4 pointer-sized parameters, the C++ function-pointer typedef must be `__stdcall` with exactly 4 parameters. The callee cleans the stack, so the caller does not need an `__declspec(naked)` shim or manual stack adjustment.

---

## 5. Mirror cross-reference: `_SaveTextEX@16` (CORRECTS FEASIBILITY §5.1)

### 5.1 SaveTextEX thunk (`0x10108160`)

```asm
10108163  CMP dword ptr [EBP + 0x8],0x0    ; if (param_1 == 0) goto fail
10108167  JZ 0x10108191
10108169  PUSH 0x0
1010816b  CALL 0x101a9650                  ; GetWarehouse(0)
1010817a  MOV EDX,dword ptr [EBP + 0x8]    ; EDX = param_1 (source text)
1010817d  MOV ECX,EAX                      ; ECX = warehouse (this)
10108177  PUSH dword ptr [EBP + 0x14]      ; param_4 -> FUN P5 (password)
1010817f  PUSH dword ptr [EBP + 0x10]      ; param_3 -> FUN P4 (progId)
10108182  PUSH dword ptr [EBP + 0xc]       ; param_2 -> FUN P3 (whType)
10108185  CALL 0x101a08b0                  ; FUN_101a08b0 (fastcall: ECX=this, EDX=sourceText)
1010818e  RET 0x10
```

### 5.2 SaveTextEX real impl (`FUN_101a08b0`) — decompiled (key lines)

```c
undefined4 __fastcall FUN_101a08b0(undefined4 wh_this,    // ECX = warehouse
                                   LPCSTR sourceText,     // EDX = SaveTextEX param_1
                                   int whType,            // P3  = SaveTextEX param_2
                                   int progId,            // P4  = SaveTextEX param_3
                                   void *password)        // P5  = SaveTextEX param_4
{
  local_20 = sourceText;
  local_10 = lstrlenA(sourceText);          // source text -> lstrlenA'd
  FUN_101d43a0(whType, local_60);           // TypeConvert (same as load)
  rec = FUN_101cff60(local_60, progId, 0);  // module lookup by type+progId (same as load)
  ...
  if (password != NULL) { ...encrypt... } else { FUN_101d3ea0(..., sourceText, len+1); }
  return 1;
}
```

### 5.3 The mirror

| param | `_SaveTextEX@16` (SAVE) | `_LoadTextFileEX@16` (LOAD) |
|-------|--------------------------|------------------------------|
| 1 | `LPCSTR sourceText` (lstrlenA'd; nonzero-checked by thunk) | `void* unused` (NULL) |
| 2 | `int whType` (TypeConvert) | `int whType` (TypeConvert) |
| 3 | `int progId` (module lookup) | `int progId` (module lookup) |
| 4 | `void* password` (NULL = standard) | `void* password` (NULL = standard) |
| return | `int` (1 = success) | `HGLOBAL` (source text; NULL = failure) |

**Parameters 2, 3, and 4 are identical** across the two functions. The only difference is param_1: SaveTextEX *takes* the source text as input; LoadTextFileEX has it unused and *returns* the source text as an `HGLOBAL`. This is the textbook load/save mirror.

> **Correction to FEASIBILITY.md §5.1:** The SaveTextEX row currently reads `void* _SaveTextEX@16(void* p1, const char* sourceTextAnsi, const char* name, void* p4)` with "p2 = source text, p3 = name". **This is incorrect.** The decompilation shows SaveTextEX param_1 (not param_2) is the source text (`lstrlenA`'d, nonzero-checked), param_2 is `whType` (TypeConvert), param_3 is `progId` (module lookup), and param_4 is the password. The correct signature is `int __stdcall _SaveTextEX@16(const char* sourceText, int whType, int progId, void* password)`. (This matches the comment in `archive/live_probe.cpp` line 43: `(const char* sourceText, int whType, int progId, void* buffer)`.) The project's production code does not call `_SaveTextEX@16` directly (it uses `_SaveModule_New@20` + a hidden EDIT control instead), so this correction has no impact on existing functionality — but it should be noted if SaveTextEX is ever used directly.

---

## 6. Recommended C++ function-pointer typedef

```cpp
// _LoadTextFileEX@16 — stdcall, 4 pointer-sized params, returns HGLOBAL.
//   param_1 (unused) : pass nullptr
//   param_2 (whType) : warehouse type code (0=Strategy, 2=Indicator,
//                      4=Function, 5122=TradingApp)
//   param_3 (progId) : positive ProgId from FindExistPos
//   param_4 (metadata/decryption buffer): nullptr for flag-clear raw storage;
//                      for internally encrypted storage (master flag 0x400),
//                      pass a zeroed writable 64-byte buffer as TSDev does
// Returns: HGLOBAL containing ANSI plaintext after the appropriate path
//          (GlobalAlloc'd with GMEM_ZEROINIT). Use GlobalLock to obtain the
//          text pointer, GlobalUnlock/GlobalFree to release. NULL = failure.
typedef HGLOBAL (__stdcall *LoadTextFileExFn)(void* /*unused*/,
                                             int   /*whType*/,
                                             int   /*progId*/,
                                             void* /*metadataBuffer*/);
```

**Usage pattern (mirrors the existing `saveSource` SEH guard in `tsdev_lib.cpp`):**
```cpp
std::array<unsigned char, 64> metadata{};
// Obtain this from the warehouse master record/API, never from source text.
void* metadataBuffer = (masterFlags & 0x0400) ? metadata.data() : nullptr;

HGLOBAL hMem = nullptr;
__try {
    hMem = m_pLoadTextFileEx(nullptr, getCategory(type), progId, metadataBuffer);
} __except (EXCEPTION_EXECUTE_HANDLER) {
    hMem = nullptr;
}
if (hMem == nullptr) {
    return "";
}
LPCSTR text = static_cast<LPCSTR>(GlobalLock(hMem));
std::string source(text ? text : "");
GlobalUnlock(hMem);
GlobalFree(hMem);
return source;
```

---

## 7. Caveats / uncertainties

1. **Corrected argument-4 semantics:** A non-NULL writable buffer selects the decryption/metadata path; for internally encrypted storage TSDev passes a zeroed 64-byte buffer, and the loader populates metadata from the source header while returning plaintext. NULL selects the raw-copy path and is correct only for flag-clear storage. This is internal storage behavior, not user password protection. Select the path from warehouse master-record metadata (`0x400` flag), never from source text.

2. **No GUI dialog in the load path:** Unlike some orlib20 functions that pop a GUI dialog (e.g. password prompts), the decompiled `FUN_101a0f90` contains **no `MessageBox`/`DialogBox` calls** — the password is taken from the caller-supplied buffer, not prompted. With `password=NULL` the path is a pure warehouse read + `GlobalAlloc` + `memcpy`. **This is safe for headless mode** (no hang risk from a modal dialog on the load path). The only blocking primitive is the warehouse command-0x11 event wait (`WaitForSingleObject` in `FUN_101d0e80`), which is satisfied by the warehouse worker thread — the same mechanism already used by the 3 existing thread-check-INT3 patches.

3. **Thread-check INT3 (command 0x11):** `FUN_101d0e80` sets `obj[4] = 0x11` (the warehouse command) and contains a `swi(3)` (INT3) thread-check at the point where `GetCurrentThreadId() != obj[0x76]`:
   ```c
   if (GetCurrentThreadId() != obj[0x76]) { swi(3); }   // INT3 — will crash headless
   ```
   This is the **same pattern** as the three existing patches (commands 0x0F/0x1E/0x17, signatures `FUN_101d0950`/`FUN_101cf5b0`/`FUN_101d1fa0`). A new NOP-patch signature for the command-0x11 INT3 is required before calling `_LoadTextFileEX@16` headless. **Identifying that patch site is a separate, owned subtask (out of scope for this findings document).**

4. **`GlobalAlloc` flag `0x40` (fixed):** The allocation is `GMEM_ZEROINIT` fixed (not moveable). `GlobalLock` is still the correct, portable way to obtain the pointer (it returns the handle cast to a pointer for fixed allocations). `GlobalFree` is required to release the memory. The implementation writes directly into the handle via `memcpy`, confirming it is directly dereferenceable.

5. **No internal thunk callers:** There are no call-site xrefs to the `_LoadTextFileEX@16` thunk from within orlib20.dll (only the export-table DATA ref). The authoritative caller evidence comes from `FUN_10108630`, which calls the real implementation `FUN_101a0f90` directly (bypassing the thunk) and calls `_SaveTextEX@16` via the thunk — both in the same function, providing the mirror confirmation. External callers (TSDev.exe) were not traced in this static-only investigation, but the static evidence is conclusive.

6. **Return buffer layout:** `GlobalAlloc(0x40, len + 10)` allocates `len + 10` bytes but `memcpy(_Dst, _Src, len)` copies only `len` bytes of source text. The extra ~10 bytes are zero-filled (GMEM_ZEROINIT) and serve as trailing slack. The text is ANSI (the save counterpart uses `lstrlenA`/`memcpy` of ANSI bytes). Treat the locked pointer as a NUL-terminated ANSI `char*` (the save path writes `len+1` bytes including the NUL; the load path copies `len` bytes into a zero-initialized buffer, so a NUL terminator is present in the slack).

---

## 8. Summary

| Question | Answer | Confidence |
|----------|--------|------------|
| **1. Parameter order** | `(void* unused, int whType, int progId, void* password)` — CONFIRMED, matches FEASIBILITY §5.1 hypothesis exactly. | **HIGH** (decompilation + authoritative internal caller) |
| **2. Return semantics** | `HGLOBAL` from `GlobalAlloc(0x40, len+10)`; `GlobalLock` → ANSI text pointer; `GlobalFree` → release; `NULL` = failure. | **HIGH** (decompilation + caller uses `GlobalFree`) |
| **3. Calling convention** | `__stdcall`, `@16` = 4 × 4-byte params, callee-cleaned (`RET 0x10`). Callee preserves EBX/ESI/EDI. | **HIGH** (thunk disasm + `RET 0x10`) |

**Recommended typedef:**
```cpp
typedef HGLOBAL (__stdcall *LoadTextFileExFn)(void* /*unused*/,
                                             int   /*whType*/,
                                             int   /*progId*/,
                                             void* /*password*/);
```

**Mirror:** `_SaveTextEX@16(sourceText, whType, progId, password)` — params 2–4 identical; param_1 is the source text (input) for save vs. unused for load (output is the HGLOBAL return). This also corrects the FEASIBILITY §5.1 SaveTextEX signature (which mislabelled param_2 as source text and param_3 as name).
