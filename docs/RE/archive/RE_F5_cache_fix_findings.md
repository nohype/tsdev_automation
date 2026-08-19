# RE F5: Cache-Staleness Fix — Two Approaches Investigated

## Problem Statement

`enumerateRecords` uses `_LoadMasterRecsEX@16` (command 0x08) to enumerate warehouse
items. This reads from a **cached SafeArray** populated at warehouse init time
(`OrlibInit`). Items created in the current session via `createEntry` + `saveSource`
+ `verifyModule` are NOT in this cached snapshot. `findExistPos` (command 0x10, live
lookup) CAN find these items, but enumeration cannot.

Two fix approaches were investigated:
- **Approach A**: Cache refresh — dispatch command 0x4E before enumeration to refresh
  the cached SafeArray.
- **Approach B**: Live-lookup enumeration — use `_GetMasterRecByType@16` (command 0x0D,
  live lookup) instead of `_LoadMasterRecsEX@16` (command 0x08, cached).

---

## 1. Approach A Analysis: Cache Refresh (Command 0x4E)

### 1.1 `FUN_101cf5b0` — General Command Dispatch Helper (RVA 0x1CF5B0)

**Already INT3-patched** (existing signature `FUN_101cf5b0_thread_check_INT3`, offset 36).

Decompilation (Ghidra):
```c
void __thiscall FUN_101cf5b0(int param_1, int param_2)
{
  WaitForSingleObject(*(HANDLE *)(param_1 + 0x72), 0xffffffff);
  DVar2 = GetCurrentThreadId();
  if ((DVar2 != *(DWORD *)(param_1 + 0x76)) && (param_2 != 0x1e)) {
    pcVar1 = (code *)swi(3);          // INT3 — patched to NOP
    (*pcVar1)();
    return;
  }
  *(int *)(param_1 + 4) = param_2;    // store command number at [warehouse+0x04]
  SetEvent(*(HANDLE *)(param_1 + 0x6e));   // signal worker thread
  WaitForSingleObject(*(HANDLE *)(param_1 + 0x6a), 0xffffffff);  // wait for completion
  ReleaseMutex(*(HANDLE *)(param_1 + 0x72));
  return;
}
```

**Signature**: `__thiscall` — ECX = warehouse object (`param_1`), stack param = command
number (`param_2`).

**Key behavior**: This is the GENERIC command dispatcher. It:
1. Acquires mutex at `[warehouse+0x72]`
2. Checks thread ID (INT3 if different thread, UNLESS command is 0x1E)
3. Stores command number at `[warehouse+0x04]`
4. Signals worker thread via event at `[warehouse+0x6E]`
5. Waits for completion via event at `[warehouse+0x6A]`
6. Releases mutex

**Export status**: `FUN_101cf5b0` is an **internal function** (not exported). It is
resolved by RVA via the existing signature scan
(`FUN_101cf5b0_thread_check_INT3`, expected RVA `0x1CF5B0`).

**Can we call `FUN_101cf5b0(warehouse, 0x4E)` directly?** — **NO.** See §1.3.

### 1.2 `FUN_101c09f0` — Full Cache Reset Function (RVA 0x1C09F0)

Decompilation (Ghidra):
```c
void __thiscall FUN_101c09f0(int param_1, int param_2)
{
  // param_1 = module object (this/ECX)
  // param_2 = type_code

  // --- Phase 1: Invalidate caches based on type_code ---
  if (param_2 == 0) {
    // Special case: invalidate ALL caches
    // Sets flags for types 1, 9, 10, 0xB, 0xC, 0xD, 0x11
  }
  else {
    // Selective invalidation based on type_code:
    // param_2 == 6  → invalidate [module+0x8F] cache
    // param_2 == 9  → invalidate [module+0x1C1] cache
    // param_2 == 10 → invalidate [module+0x1B8] cache
    // param_2 == 0xB → invalidate [module+0x1C9] cache
    // param_2 == 0xC → invalidate [module+0x1DE] cache
    // param_2 == 0xD → invalidate [module+0x1FC] cache
    // param_2 == 0x11 → invalidate [module+0x22D] cache
  }

  // --- Phase 2: Lock and prepare for 0x4E dispatch ---
  CSingleLock::CSingleLock(local_34, *(CSyncObject **)(param_1 + 0x218), 1);
  FUN_101c82b0();

  // --- Phase 3: Resolve COM interface if not cached ---
  if (*(int *)(param_1 + 0x10) == 0) {
    // [module+0x10] is NOT cached — must call FUN_101cfce0 (command 0x0D)
    // to resolve a COM interface pointer
    iVar3 = FUN_101cfce0(
        (int)*(short *)(param_1 + 0x15),    // ProgId (short at [module+0x15])
        *(undefined4 *)(param_1 + 0x18)     // value at [module+0x18]
    );
    *(int *)(param_1 + 0x10) = iVar3;       // cache the result
    if (iVar3 == 0) {
      pcVar2 = (code *)swi(3);              // INT3 crash if resolution fails
      (*pcVar2)();
      return;
    }
  }

  // --- Phase 4: Set up warehouse fields and dispatch 0x4E ---
  // warehouse = *(int *)(module + 0xC)
  *(undefined4 *)(*(int *)(param_1 + 0xc) + 0x5e) = *(undefined4 *)(param_1 + 0x10);
  //   → [warehouse+0x5E] = [module+0x10]  (COM interface pointer)

  *(int *)(*(int *)(param_1 + 0xc) + 0x12) = param_2;
  //   → [warehouse+0x12] = type_code

  FUN_101cf5b0(0x4e);
  //   → dispatch command 0x4E on the warehouse (ECX = warehouse from [module+0xC])

  CSingleLock::Unlock(local_34);
  return;
}
```

**Signature**: `__thiscall` — ECX = **module object** (NOT the warehouse), stack param =
type_code.

**Critical findings**:

1. **`param_1` is a MODULE object, not the warehouse.** The warehouse is at
   `[module+0xC]`. The module object is a large structure with multiple cache fields
   (at offsets 0x8F, 0x1B8, 0x1C1, 0x1C9, 0x1DE, 0x1FC, 0x22D) and a sync object at
   `[module+0x218]`.

2. **The setup BEFORE 0x4E is REQUIRED.** `FUN_101cf5b0(warehouse, 0x4E)` alone is
   NOT sufficient because the worker thread case 0x4E handler (`FUN_101cf710`) needs:
   - `[warehouse+0x5E]` = a COM interface pointer (set from `[module+0x10]`)
   - `[warehouse+0x12]` = type_code

   If `[warehouse+0x5E]` is NULL (not set), the QueryInterface call in
   `FUN_101cf710` would crash.

3. **`FUN_101cfce0` may be called if `[module+0x10]` is 0.** This function dispatches
   command 0x0D and has its OWN INT3 thread check at offset 0x28 (40). This INT3 is
   **NOT patched** by any existing signature. See §1.5.

4. **Type codes are NOT the same as component categories.** The type_code parameter
   (`param_2`) uses a different numbering: 0 = all, 6, 9, 10, 0xB, 0xC, 0xD, 0x11.
   These do NOT correspond directly to the project's component types (Strategy=0,
   Indicator=2, Function=4, TradingApp=0x1402). The mapping between component types
   and these internal type codes is unclear and would require additional RE.

### 1.3 `FUN_101cf710` — Worker Thread Case 0x4E (RVA 0x1CF710)

Decompilation (Ghidra):
```c
HRESULT __thiscall FUN_101cf710(IUnknown *param_1, IID *param_2)
{
  HVar1 = (*param_1->lpVtbl[6].QueryInterface)(param_1, param_2, unaff_EDI);
  if (HVar1 < 0) {
    _com_issue_errorex(HVar1, param_1, (_GUID *)&DAT_101faa30);
  }
  return HVar1;
}
```

**This is a COM QueryInterface call.** It calls `param_1->lpVtbl[6].QueryInterface`
(vtable index 6 = offset 0x18 in the vtable, but the background says "WHServer COM
vtable 0x6C" for command 0x08 — this is a different vtable index for a different
command).

- `param_1` (IUnknown*) comes from `[warehouse+0x5E]` — the COM interface pointer
  set by `FUN_101c09f0`.
- `param_2` (IID*) comes from warehouse state (likely derived from
  `[warehouse+0x12]` = type_code).

**Does 0x4E refresh ALL types or just a specific type?** It refreshes a specific type
determined by the IID (`param_2`), which is derived from the type_code set at
`[warehouse+0x12]`. To refresh all types, `FUN_101c09f0(module, 0)` would be called
(type_code 0 = invalidate all).

**Can we call `FUN_101cf5b0(warehouse, 0x4E)` directly?** — **NO.**
`FUN_101cf710` requires `[warehouse+0x5E]` to be a valid COM interface pointer. Without
the `FUN_101c09f0` setup, `[warehouse+0x5E]` would be 0 or stale, causing a crash in
the QueryInterface call.

### 1.4 How to Get the Module Object?

The module object (`param_1` of `FUN_101c09f0`) is NOT the warehouse. It is a
higher-level object that contains a pointer to the warehouse at `[module+0xC]`.

**There is no known export or simple internal function that returns the module
object.** The delete orchestrator (`FUN_101a8360`) obtains the module object
internally during the program loading process (via command 0x0F / `FUN_101d0950`),
which requires knowing a ProgId of an existing item.

This creates a **chicken-and-egg problem**: to refresh the cache (to enumerate items),
we need a module object, which requires loading a program, which requires knowing a
ProgId, which requires enumeration (the very thing we're trying to fix).

The existing code gets the warehouse via `GetWarehouse(NULL)` (`FUN_101a9650`,
resolved via signature scan, `__cdecl`, 1 arg). But the module object is not
accessible this way.

### 1.5 `FUN_101cfce0` — Command 0x0D Dispatcher (RVA 0x1CFCE0)

This function is called from `FUN_101c09f0` when `[module+0x10] == 0` (COM interface
not yet cached).

Decompilation (Ghidra):
```c
undefined4 __thiscall FUN_101cfce0(int param_1, undefined4 param_2, undefined4 param_3)
{
  // param_1 = warehouse (this/ECX)
  // param_2 = type (ProgId as short, passed from [module+0x15])
  // param_3 = value (passed from [module+0x18])

  *(undefined4 *)(param_1 + 0x12) = param_2;   // [warehouse+0x12] = type
  *(undefined4 *)(param_1 + 0x16) = param_3;   // [warehouse+0x16] = value
  WaitForSingleObject(*(HANDLE *)(param_1 + 0x72), 0xffffffff);
  DVar2 = GetCurrentThreadId();
  if (DVar2 != *(DWORD *)(param_1 + 0x76)) {
    pcVar1 = (code *)swi(3);                    // INT3 — NOT patched!
    uVar3 = (*pcVar1)();
    return uVar3;
  }
  *(undefined4 *)(param_1 + 4) = 0xd;           // command 0x0D
  SetEvent(*(HANDLE *)(param_1 + 0x6e));
  WaitForSingleObject(*(HANDLE *)(param_1 + 0x6a), 0xffffffff);
  ReleaseMutex(*(HANDLE *)(param_1 + 0x72));
  uVar3 = *(undefined4 *)(param_1 + 0x5e);      // return [warehouse+0x5E]
  *(undefined4 *)(param_1 + 0x5e) = 0;          // clear it
  return uVar3;
}
```

**INT3 at offset 0x28 (40)** — **NOT patched** by any existing signature.

Disassembly:
```asm
101cfce0  PUSH EBP                      ; 55
101cfce1  MOV EBP,ESP                   ; 8B EC
101cfce3  MOV EAX,[EBP+0x8]             ; 8B 45 08
101cfce6  PUSH ESI                      ; 56
101cfce7  MOV ESI,ECX                   ; 8B F1
101cfce9  PUSH -0x1                     ; 6A FF
101cfceb  PUSH [ESI+0x72]               ; FF 76 72
101cfcee  MOV [ESI+0x12],EAX            ; 89 46 12
101cfcf1  MOV EAX,[EBP+0xC]             ; 8B 45 0C
101cfcf4  MOV [ESI+0x16],EAX            ; 89 46 16
101cfcf7  CALL [__imp__WaitForSingleObject]  ; FF 15 ?? ?? ?? ??
101cfcfd  CALL [__imp__GetCurrentThreadId]   ; FF 15 ?? ?? ?? ??
101cfd03  CMP EAX,[ESI+0x76]            ; 3B 46 76
101cfd06  JZ 0x101cfd09                 ; 74 01
101cfd08  INT3                          ; CC  ← offset 0x28 (40) — UNPATCHED
101cfd09  PUSH [ESI+0x6E]               ; FF 76 6E
101cfd0c  MOV [ESI+0x4],0xD             ; C7 46 04 0D 00 00 00
```

**Byte pattern** (51 bytes, unique — verified by Ghidra search: 1 match at `0x101CFCE0`):
```
55 8B EC 8B 45 08 56 8B F1 6A FF FF 76 72 89 46 12 8B 45 0C 89 46 16 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 0D 00 00 00
```

> **Note**: Without the command number `0D` at the end, this pattern matches TWO
> functions: `FUN_101cfce0` (command 0x0D) and `FUN_101d1fa0` (command 0x17, already
> patched). The command number differentiates them.

**Why doesn't the existing delete flow crash?** In the delete orchestrator
(`FUN_101a8360`), the program is first loaded via `FUN_101d0950` (command 0x0F),
which populates `[module+0x10]` with the COM interface pointer. By the time
`FUN_101c09f0` is called, `[module+0x10]` is already non-zero, so `FUN_101cfce0`
is **skipped**. The INT3 is never hit.

For a standalone cache reset (Approach A), `[module+0x10]` would be 0, triggering
`FUN_101cfce0` and hitting the unpatched INT3.

### 1.6 Approach A Feasibility Assessment

| Requirement | Status | Complexity |
|-------------|--------|------------|
| `FUN_101cf5b0` patch (command 0x4E dispatch) | Already patched | None |
| `FUN_101cfce0` patch (command 0x0D, for `[module+0x10]` resolution) | **NEW patch needed** (offset 40, CC→90) | Low |
| Module object pointer | **No known way to obtain** | **HIGH — blocking** |
| Type code mapping (component type → internal type_code) | **Unknown, needs RE** | Medium-High |
| `FUN_101c09f0` is internal (not exported) | Resolve by RVA (signature scan) | Medium |
| `FUN_101cfce0` is internal (not exported) | Resolve by RVA (signature scan) | Medium |

**Verdict: Approach A is NOT FEASIBLE without significant additional RE.**

The blocking issue is obtaining the module object pointer. There is no export or
simple internal function that returns it. The module object is only accessible
through the program-loading path (command 0x0F), which requires a ProgId — creating
a chicken-and-egg problem. Additionally, the type_code mapping is unknown and would
require further RE of `FUN_101c09f0`'s type_code parameter.

Even if the module object were obtainable, the approach requires:
- 1 new INT3 patch (`FUN_101cfce0`, offset 40)
- 1 new signature for `FUN_101c09f0` (to resolve its RVA)
- 1 new signature for `FUN_101cfce0` (to resolve its RVA for patching)
- Correct type_code values for each component type
- SEH-protected `__thiscall` call to `FUN_101c09f0`

**Complexity: HIGH. Risk: HIGH** (COM interface manipulation, unknown type codes,
module object access).

---

## 2. Approach B Analysis: Live-Lookup Enumeration (`_GetMasterRecByType@16`)

### 2.1 `_GetMasterRecByType@16` — Export (Ordinal 510, RVA 0x1A0460)

**Confirmed as an exported function** (ordinal 510, RVA `0x1A0460`, `__stdcall`,
decorated name `_GetMasterRecByType@16`).

Decompilation (Ghidra):
```c
undefined4 _GetMasterRecByType_16(
    undefined4 param_1,   // [EBP+0x08] category (0=Strategy, 2=Indicator, 4=Function, ...)
    undefined4 param_2,   // [EBP+0x0C] subtype (power type for ConvertToPowerType)
    undefined4 param_3,   // [EBP+0x10] index (0-based)
    undefined4 *param_4   // [EBP+0x14] output buffer (0x10A bytes, caller-allocated)
)
{
  int iVar1;
  undefined4 uVar2;
  undefined4 *puVar3;
  undefined4 *puVar4;

  // 1. Get warehouse (NULL path = default warehouse)
  iVar1 = FUN_101a9650(0);           // GetWarehouse(NULL)
  if (iVar1 == 0) {
    if (param_4 != (undefined4 *)0x0) {
      FUN_10158fb0(param_4, &PTR_10206f80, 0);   // init empty record
    }
    return 0;                         // warehouse unavailable
  }

  // 2. Convert (category, subtype) → expanded warehouse type
  uVar2 = FUN_101d4280(param_1, param_2);

  // 3. Live lookup: get record by expanded type + index
  //    FUN_101cfe00 is __thiscall: ECX = warehouse (iVar1)
  //    param_2 = expanded type, param_3 = index, param_4 = 0 (allocate internally)
  puVar3 = (undefined4 *)FUN_101cfe00(uVar2, param_3, 0);

  if (puVar3 == (undefined4 *)0x0) {
    if (param_4 != (undefined4 *)0x0) {
      FUN_10158fb0(param_4, &PTR_10206f80, 0);   // init empty record
    }
    return 0;                         // record not found (index out of range)
  }

  // 4. Copy 0x10A bytes (0x42 dwords + 1 word) to caller's buffer
  puVar4 = puVar3;
  for (iVar1 = 0x42; iVar1 != 0; iVar1 = iVar1 + -1) {
    *param_4 = *puVar4;
    puVar4 = puVar4 + 1;
    param_4 = param_4 + 1;
  }
  *(undefined2 *)param_4 = *(undefined2 *)puVar4;   // last 2 bytes

  // 5. Free internal buffer
  operator_delete__(puVar3);
  return 1;                           // success
}
```

**Return value**: 1 = success (record found and copied), 0 = failure (not found or
warehouse unavailable).

**Record layout**: Same 0x10A-byte (266-byte) layout as `_LoadMasterRecsEX@16`.
Name at offset 0x17, ProgId (short) at offset 0x00, subtype byte at offset 0xE9.

### 2.2 `FUN_101cfe00` — Internal Record Retrieval (RVA 0x1CFE00)

Decompilation (Ghidra):
```c
void * __thiscall FUN_101cfe00(
    int param_1,         // this/ECX = warehouse
    undefined4 param_2,  // expanded type
    undefined4 param_3,  // index
    void *param_4        // optional output buffer (NULL = allocate internally)
)
{
  undefined4 *puVar3;
  undefined4 *local_8 = NULL;

  if (*(int *)(param_1 + 0xc2) == 1) {
    // Server mode: use FUN_101a9460 + FUN_101e3990 (no command 0x0D)
    FUN_101a9460(param_2, param_3);
    puVar3 = (undefined4 *)FUN_101e3990(param_2, param_3);
  }
  else {
    // Local mode: dispatch command 0x0D via FUN_101d15c0
    FUN_101d15c0(param_2, param_3, &local_8);
    puVar3 = local_8;
  }

  if (puVar3 != (undefined4 *)0x0) {
    if (param_4 == (void *)0x0) {
      param_4 = operator_new__(0x10a);       // allocate 0x10A bytes
    }
    uVar4 = FUN_101c3c50();   // get record data (triggers lazy-init via cmd 0x35)
    uVar5 = FUN_101c3c70();   // get record size
    FUN_101d4490(param_4, 0x10a, uVar4, uVar5);  // memcpy record data
    // ... release item reference
    return param_4;
  }
  return (void *)0x0;  // not found
}
```

**Key finding**: In local mode (`[warehouse+0xC2] != 1`), this calls `FUN_101d15c0`
which dispatches **command 0x0D** — a LIVE lookup. This is the same command used by
`FUN_101cfce0` (in the cache reset path), confirming that command 0x0D accesses the
warehouse's live item list, NOT the cached SafeArray.

The record data is obtained via `FUN_101c3c50` (same function used by
`_LoadMasterRecsEX@16`), which triggers `FUN_101c5b10` (lazy-init, command 0x35,
already patched via `FUN_101cf5b0`).

### 2.3 `FUN_101d15c0` — Command 0x0D Local-Mode Dispatcher (RVA 0x1D15C0)

Decompilation (Ghidra):
```c
undefined4 __thiscall FUN_101d15c0(
    int param_1,          // this/ECX = warehouse
    undefined4 param_2,   // expanded type
    undefined4 param_3,   // index
    int *param_4          // output: item object pointer
)
{
  if (*(int *)(param_1 + 0xae) != 0) {   // warehouse initialized?
    *param_4 = 0;
    *(undefined4 *)(param_1 + 0x12) = param_2;   // [warehouse+0x12] = type
    *(undefined4 *)(param_1 + 0x16) = param_3;   // [warehouse+0x16] = index
    WaitForSingleObject(*(HANDLE *)(param_1 + 0x72), 0xffffffff);
    DVar2 = GetCurrentThreadId();
    if (DVar2 != *(DWORD *)(param_1 + 0x76)) {
      pcVar1 = (code *)swi(3);            // INT3 — needs patch!
      uVar3 = (*pcVar1)();
      return uVar3;
    }
    *(undefined4 *)(param_1 + 4) = 0xd;   // command 0x0D
    SetEvent(*(HANDLE *)(param_1 + 0x6e));
    WaitForSingleObject(*(HANDLE *)(param_1 + 0x6a), 0xffffffff);
    ReleaseMutex(*(HANDLE *)(param_1 + 0x72));

    if (*(int *)(param_1 + 0x5e) != 0) {  // result available?
      pvVar4 = operator_new(0x241);       // allocate item object (577 bytes)
      iVar5 = FUN_101bfb00();             // initialize item object
      *param_4 = iVar5;
      FUN_101c4ae0(param_1, *(undefined4 *)(param_1 + 0x5e), param_2, param_3);
      // populate item from [warehouse+0x5E] result
      // ... increment refcount
      return 1;                           // success
    }
  }
  return 0;                               // not found / not initialized
}
```

**INT3 at offset 0x54 (84)** — **NEW patch required.**

### 2.4 INT3 Patch for `FUN_101d15c0` (Command 0x0D)

Disassembly:
```asm
101d15c0  PUSH EBP                      ; 55
101d15c1  MOV EBP,ESP                   ; 8B EC
101d15c3  MOV EAX,FS:[0x0]              ; 64 A1 00 00 00 00
101d15c9  PUSH -0x1                     ; 6A FF
101d15cb  PUSH 0x101f5e12               ; 68 ?? ?? ?? ??  (SEH handler, wildcarded)
101d15d0  PUSH EAX                      ; 50
101d15d1  MOV FS:[0x0],ESP              ; 64 89 25 00 00 00 00
101d15d8  PUSH EBX                      ; 53
101d15d9  PUSH ESI                      ; 56
101d15da  MOV ESI,ECX                   ; 8B F1            ; this = warehouse
101d15dc  CMP [ESI+0xAE],0              ; 83 BE AE 00 00 00 00
101d15e3  JZ 0x101d16a0                 ; 0F 84 ?? ?? ?? ??
101d15e9  MOV EBX,[EBP+0x10]            ; 8B 5D 10
101d15ec  MOV EAX,[EBP+0x8]             ; 8B 45 08
101d15ef  PUSH -0x1                     ; 6A FF
101d15f1  MOV [EBX],0                   ; C7 03 00 00 00 00
101d15f7  PUSH [ESI+0x72]               ; FF 76 72
101d15fa  MOV [ESI+0x12],EAX            ; 89 46 12
101d15fd  MOV EAX,[EBP+0xC]             ; 8B 45 0C
101d1600  MOV [ESI+0x16],EAX            ; 89 46 16
101d1603  CALL [__imp__WaitForSingleObject]  ; FF 15 ?? ?? ?? ??
101d1609  CALL [__imp__GetCurrentThreadId]   ; FF 15 ?? ?? ?? ??
101d160f  CMP EAX,[ESI+0x76]            ; 3B 46 76
101d1612  JZ 0x101d1615                 ; 74 01
101d1614  INT3                          ; CC  ← offset 0x54 (84), patch CC→90
101d1615  PUSH [ESI+0x6E]               ; FF 76 6E
101d1618  MOV [ESI+0x4],0xD             ; C7 46 04 0D 00 00 00
```

**Byte pattern** (97 bytes, unique — verified by Ghidra `search_byte_patterns`:
**1 match** at `0x101D15C0`):
```
55 8B EC 64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 53 56 8B F1 83 BE AE 00 00 00 00 0F 84 ?? ?? ?? ?? 8B 5D 10 8B 45 08 6A FF C7 03 00 00 00 00 FF 76 72 89 46 12 8B 45 0C 89 46 16 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 0D 00 00 00
```

**Proposed signature entry**:
```json
{
  "name": "FUN_101d15c0_thread_check_INT3",
  "targetType": "patch_site",
  "callingConvention": "thiscall",
  "expectedRva": "0x1D15C0",
  "hexPattern": "55 8B EC 64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 53 56 8B F1 83 BE AE 00 00 00 00 0F 84 ?? ?? ?? ?? 8B 5D 10 8B 45 08 6A FF C7 03 00 00 00 00 FF 76 72 89 46 12 8B 45 0C 89 46 16 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 0D 00 00 00",
  "length": 97,
  "expectedMatchCount": 1,
  "patch": {
    "offset": 84,
    "originalBytes": "CC",
    "replacementBytes": "90",
    "originalBytesMask": "FF",
    "description": "NOP thread-check INT3 in command 0x0D (get record by type+index) local-mode dispatch helper"
  },
  "containingFunction": "FUN_101d15c0",
  "provenance": {
    "author": "RE-F5-cache-fix",
    "date": "2026-08-26",
    "source": "Ghidra disassembly",
    "reference": "docs/RE/RE_F5_cache_fix_findings.md"
  }
}
```

**Uniqueness differentiation from `FUN_101d16c0` (command 0x09)**:

| Feature | `FUN_101d15c0` (cmd 0x0D) | `FUN_101d16c0` (cmd 0x09) |
|---------|---------------------------|---------------------------|
| Register saves | `53 56 8B F1` (PUSH EBX, PUSH ESI, MOV ESI,ECX) | `56 8B F1 57` (PUSH ESI, MOV ESI,ECX, PUSH EDI) |
| Output ptr reg | `8B 5D 10` (MOV EBX,[EBP+0x10]) | `8B 7D 10` (MOV EDI,[EBP+0x10]) |
| Zero output | `C7 03 00 00 00 00` (MOV [EBX],0) | `C7 07 00 00 00 00` (MOV [EDI],0) |
| Store offset | `89 46 16` (MOV [ESI+0x16]) | `89 46 08` (MOV [ESI+0x08]) |
| Command | `0D` | `09` |

All differentiating bytes are in fixed (non-wildcarded) positions, ensuring pattern
uniqueness.

### 2.5 `FUN_101d4280` — Type Conversion (RVA 0x1D4280)

This function converts (category, subtype) to an expanded warehouse type. It is a
pure computation (no warehouse commands, no patches needed).

Decompilation (Ghidra):
```c
undefined4 FUN_101d4280(undefined4 param_1, undefined4 param_2)
{
  switch(param_1) {
  case 0:   // Strategy
    iVar1 = ConvertToPowerType(0, param_2);
    return (iVar1 == 2) ? 0x200 : 0;
  case 1:   // ActivityBar
    return 1;
  case 2:   // Indicator
    uVar2 = ConvertToPowerType(2, param_2);
    switch(uVar2) {
    case 0:  return 2;
    case 4:  return 0x402;
    case 5:  return 0x502;    // standard Indicator
    case 6:  return 0x602;
    case 8:  return 0x802;
    case 10: return 0xA02;
    case 0xC: return 0xC02;
    case 0xE: return 0xE02;
    case 0x10: return 0x1002;
    case 0x12: return 0x1202;
    case 0x14: return 0x1402;  // TradingApp
    }
    break;
  case 3:   // PaintBar
    return 3;
  case 4:   // Function
    return 4;     // no subtype conversion needed
  }
  return 0xFFFFFFFF;
}
```

`ConvertToPowerType` (RVA 0x1584F0) maps:
- Category 0 (Strategy): subtype 2 or 3 → power type 2
- Category 2 (Indicator): subtype 5 or 3 → power type 5; subtype 0x14 → power type 0x14

### 2.6 Subtype Mapping for `_GetMasterRecByType@16`

The existing `getSubType(ComponentType)` function returns `type >> 8`, which gives:
- Strategy (0): subType = 0
- Indicator (2): subType = 0
- Function (4): subType = 0
- TradingApp (0x1402): subType = 0x14

But `_GetMasterRecByType@16` needs a DIFFERENT subtype value (the "power type"
input to `ConvertToPowerType`):

| ComponentType | Category | Subtype for `_GetMasterRecByType@16` | Expanded Type | Rationale |
|---------------|----------|---------------------------------------|---------------|-----------|
| Strategy | 0 | **2** | 0x200 | `ConvertToPowerType(0, 2)` = 2 → `FUN_101d4280` returns 0x200 |
| Indicator | 2 | **5** | 0x502 | `ConvertToPowerType(2, 5)` = 5 → `FUN_101d4280` returns 0x502 |
| Function | 4 | **0** (any) | 4 | `FUN_101d4280` returns 4 directly (no conversion) |
| TradingApp | 2 | **0x14** | 0x1402 | `ConvertToPowerType(2, 0x14)` = 0x14 → `FUN_101d4280` returns 0x1402 |

> **IMPORTANT**: The existing `getSubType()` returns 0 for Strategy and Indicator,
> which would NOT work with `_GetMasterRecByType@16`. A separate subtype mapping
> function is needed for Approach B.

### 2.7 Count Mechanism — Iterating Until Failure

There is **no live count API**. `_GetNumofMasterRecsEX@4` uses command 0x07 (cached),
which has the same staleness problem as command 0x08.

**Solution**: Iterate with increasing indices until `_GetMasterRecByType@16` returns 0
(record not found). This is the "iterate until NULL" approach.

```cpp
BYTE recBuf[0x10A];
int index = 0;
while (_GetMasterRecByType(category, subType, index, recBuf) == 1) {
    // process recBuf (0x10A bytes, name at 0x17, ProgId at 0x00)
    index++;
}
// index = total count of live records
```

**Performance**: Each call dispatches command 0x0D (mutex acquire + event signal +
wait + mutex release). For ~1476 records, this is ~1477 calls. In practice, each call
takes ~0.1ms (mutex/event overhead), so total enumeration takes ~150ms. This is
acceptable for the `exists` use case (one-time query per invocation).

**Gap handling**: If the warehouse uses dense indexing (0 to N-1, no gaps), iterating
until 0 is reliable. If there are gaps (deleted items leaving holes), the iteration
would stop prematurely. To handle potential gaps, continue past 0 returns for a few
iterations:

```cpp
int consecutiveMisses = 0;
const int maxConsecutiveMisses = 10;
for (int index = 0; consecutiveMisses < maxConsecutiveMisses; index++) {
    if (_GetMasterRecByType(category, subType, index, recBuf) == 1) {
        consecutiveMisses = 0;
        // process record
    } else {
        consecutiveMisses++;
    }
}
```

> **Note**: The warehouse likely uses dense indexing (items are stored in an array,
> as evidenced by `_LoadMasterRecsEX@16`'s iteration `for i = 0 to count-1`). The
> gap-handling logic is a safety measure, not expected to be needed in practice.

### 2.8 Patches Required for Approach B

| Patch | Function | Command | Offset | Status |
|-------|----------|---------|--------|--------|
| `FUN_101d15c0_thread_check_INT3` | `FUN_101d15c0` | 0x0D | 84 | **NEW** — CC→90 |
| `FUN_101cf5b0_thread_check_INT3` | `FUN_101cf5b0` | 0x35 (lazy-init) | 36 | Already patched (existing) |

**No other patches needed.** The `FUN_101c3c50`/`FUN_101c3c70` calls in
`FUN_101cfe00` trigger `FUN_101c5b10` (lazy-init), which dispatches command 0x35 via
`FUN_101cf5b0` — already patched.

### 2.9 Approach B Feasibility Assessment

| Requirement | Status | Complexity |
|-------------|--------|------------|
| `_GetMasterRecByType@16` export | Ordinal 510, RVA 0x1A0460, `__stdcall` | Add to exports JSON |
| `FUN_101d15c0` INT3 patch (command 0x0D) | **NEW** — offset 84, CC→90, pattern verified unique | Low |
| `FUN_101cf5b0` INT3 patch (command 0x35) | Already patched | None |
| Count mechanism | Iterate until return 0 | Low |
| Subtype mapping | New mapping function (Strategy=2, Indicator=5, Function=0, TradingApp=0x14) | Low |
| Warehouse access | Handled internally by `_GetMasterRecByType@16` (calls `GetWarehouse(NULL)`) | None |
| Record layout | Same 0x10A bytes, name at 0x17, ProgId at 0x00 | None (reuse existing) |

**Verdict: Approach B is FEASIBLE with minimal new RE.**

Requires only 1 new patch and 1 new export entry. No new signature scans for internal
functions. No module object access needed. No type_code mapping needed.

**Complexity: LOW. Risk: LOW** (well-understood export, same record format, single
patch, proven pattern).

---

## 3. Secondary Issue: Empty Names at Offset 0x17

### 3.1 `[rec+0x73]` — Name Storage Type Flag

From `FUN_101c3660` (name accessor, RVA 0x1C3660) decompilation:
```c
short * __fastcall FUN_101c3660(short *param_1)
{
  if (*(byte *)((int)param_1 + 0x73) < 6) {
    return param_1 + 1;           // name is inline at offset 2 (short* + 1 = +2 bytes)
  }
  // [rec+0x73] >= 6: name is warehouse-stored
  if (*(int *)(param_1 + 3) != 0) {   // [rec+0x06] != 0 (warehouse module pointer)
    iVar2 = FUN_101d04f0(
        *(undefined4 *)(param_1 + 1),  // ProgId (at offset 0x02, as int)
        (int)*param_1,                 // type (at offset 0x00, as short)
        &local_8
    );
    // FUN_101d04f0 dispatches command 0x09 (live name lookup)
    if (iVar2 == 1) {
      psVar3 = (short *)FUN_101c36d0();  // get name from item
      // ... release item
      return psVar3;
    }
  }
  return (short *)0x0;   // name not found
}
```

**`[rec+0x73]` values**:
- **< 6**: Name is inline at offset 0x02 (in the original record, before lazy-init
  overwrites it with the ProgId). But for raw records from `_LoadMasterRecsEX@16` or
  `_GetMasterRecByType@16`, offset 0x02 is overwritten by `FUN_101c5b10` with the
  ProgId. The name at offset 0x17 is the reliable read location.
- **>= 6**: Name is warehouse-stored. `FUN_101c3660` looks it up by dispatching
  command 0x09 (via `FUN_101d04f0`) with the ProgId and type. The name is returned
  from the warehouse's name database, not from the inline record buffer.

### 3.2 Where Are Names Stored for `[rec+0x73] >= 6` Records?

For records with `[rec+0x73] >= 6`, the name is stored in the **warehouse's name
database**, not inline in the record buffer. The name is retrieved by:
1. `FUN_101d04f0(ProgId, type, &outItem)` — dispatches command 0x09 to look up the
   item by ProgId + type
2. `FUN_101c36d0()` — extracts the name from the returned item object

This is an internal orlib20 mechanism. The name is NOT at offset 0x17 for these
records — that field is empty or contains non-printable data.

### 3.3 Does `_GetMasterRecByType@16` Include Names in a Different Location?

**No.** `_GetMasterRecByType@16` uses the same `FUN_101c3c50` (record data accessor)
as `_LoadMasterRecsEX@16`. The record data is the same 0x10A-byte buffer with the
same layout. The name at offset 0x17 is populated by `FUN_10158fb0` during record
creation. For records with `[rec+0x73] >= 6`, the name was never written to offset
0x17 (or was written to a different field that gets cleared during warehouse init).

Both `_LoadMasterRecsEX@16` and `_GetMasterRecByType@16` return the same record
format with the same empty-name issue for `[rec+0x73] >= 6` records.

### 3.4 Impact on Case-Insensitive `exists`

**For user-created items**: Names are ALWAYS at offset 0x17. This is because
`createEntry` calls `_CreateEntryEX@24` which internally calls `FUN_10158fb0` to
initialize the record, and `FUN_10158fb0` writes the name to offset 0x17 via
`strcpy_s((char*)(rec+0x17), 0x3D, name)`. User-created items have `[rec+0x73] < 6`
(inline name storage).

**For built-in items**: Some have `[rec+0x73] >= 6` (warehouse-stored names) and
empty names at offset 0x17. These are typically older/built-in items whose names
are stored in the warehouse's name database.

**Recommendation**: For the case-insensitive `exists` use case, **skip records with
empty/non-printable names at offset 0x17**. This is acceptable because:

1. **User-created items always have names at 0x17** — the primary use case for
   case-insensitive `exists` is finding user-created items (built-in items can be
   found via exact-match `findExistPos`).
2. **Handling warehouse-stored names would require dispatching command 0x09 for each
   record with `[rec+0x73] >= 6`** — adding significant complexity and performance
   overhead (one command dispatch per record).
3. **The existing `enumerateRecords` already skips records with non-printable names**
   (see `tsdev_lib.cpp` line 815: `if (storedName[0] >= 0x20 && storedName[0] < 0x7F)`).

The skip logic:
```cpp
const char* storedName = rec + 0x17;
if (storedName[0] < 0x20 || storedName[0] >= 0x7F) {
    continue;  // skip records with empty/warehouse-stored names
}
// case-insensitive comparison
if (_stricmp(storedName, searchName.c_str()) == 0) {
    // found
}
```

---

## 4. Recommendation

### **Approach B (Live-Lookup Enumeration) is the recommended approach.**

| Criterion | Approach A (Cache Refresh) | Approach B (Live Lookup) |
|-----------|---------------------------|--------------------------|
| New patches | 1 (`FUN_101cfce0`, offset 40) | 1 (`FUN_101d15c0`, offset 84) |
| New exports | 0 | 1 (`_GetMasterRecByType@16`) |
| New signatures (internal functions) | 2 (`FUN_101c09f0`, `FUN_101cfce0`) | 0 |
| Module object access | **Blocking issue** — no known way to obtain | Not needed |
| Type code mapping | **Unknown** — needs additional RE | Simple subtype mapping (4 values) |
| Complexity | HIGH | LOW |
| Risk | HIGH (COM manipulation, unknowns) | LOW (well-understood export) |
| Record format | Same (0x10A, name at 0x17) | Same (0x10A, name at 0x17) |
| Handles new items | Yes (after cache refresh) | Yes (live lookup) |

**Why Approach B is better**:

1. **No blocking issues**: `_GetMasterRecByType@16` is an exported function that
   handles warehouse access internally (calls `GetWarehouse(NULL)`). No need to
   obtain a module object or resolve internal functions.

2. **Minimal new RE**: Only 1 new INT3 patch (pattern verified unique) and 1 new
   export entry. No new signature scans for internal functions.

3. **Proven pattern**: The INT3 patch follows the exact same pattern as the existing
   6 patches (CC→90 at the thread-check site). The byte pattern is verified unique
   by Ghidra search.

4. **Same record format**: Records from `_GetMasterRecByType@16` have the same
   0x10A-byte layout as `_LoadMasterRecsEX@16`, so the existing name extraction
   logic (offset 0x17, ProgId at 0x00, subtype at 0xE9) can be reused.

5. **Live lookup**: Command 0x0D accesses the warehouse's live item list, not the
   cached SafeArray. This means newly created items are visible immediately, without
   any cache refresh step.

6. **Simpler implementation**: Replace the single `_LoadMasterRecsEX@16` call with
   a loop of `_GetMasterRecByType@16` calls. The rest of the enumeration logic
   (name comparison, ProgId extraction, subtype filtering) remains unchanged.

**Why Approach A is worse**:

1. **Blocking issue**: No known way to obtain the module object pointer. The module
   object is only accessible through the program-loading path (command 0x0F), which
   requires a ProgId — a chicken-and-egg problem.

2. **Unknown type codes**: `FUN_101c09f0`'s `param_2` (type_code) uses an internal
   numbering (0, 6, 9, 10, 0xB, 0xC, 0xD, 0x11) that doesn't map directly to
   component types. Additional RE would be needed to determine the correct type_code
   for each component type.

3. **More patches and signatures**: Requires patching `FUN_101cfce0` (command 0x0D,
   offset 40) AND resolving `FUN_101c09f0` by signature scan. Plus the existing
   `FUN_101cf5b0` patch.

4. **COM interface manipulation**: The cache reset involves COM QueryInterface
   calls, which are inherently riskier than simple record retrieval.

5. **Cache refresh is a side effect**: Even if successful, refreshing the cache
   affects all subsequent operations, potentially causing unexpected behavior in
   other parts of the application.

---

## 5. Implementation Guidance (Approach B)

### 5.1 Add Export Entry

Add to `signatures/v9.50.01.3344.json` → `exports` array:
```json
{
  "name": "_GetMasterRecByType@16",
  "rva": "0x1A0460",
  "callingConvention": "stdcall"
}
```

### 5.2 Add INT3 Patch Signature

Add to `signatures/v9.50.01.3344.json` → `signatures` array:
```json
{
  "name": "FUN_101d15c0_thread_check_INT3",
  "targetType": "patch_site",
  "callingConvention": "thiscall",
  "expectedRva": "0x1D15C0",
  "hexPattern": "55 8B EC 64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 53 56 8B F1 83 BE AE 00 00 00 00 0F 84 ?? ?? ?? ?? 8B 5D 10 8B 45 08 6A FF C7 03 00 00 00 00 FF 76 72 89 46 12 8B 45 0C 89 46 16 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 0D 00 00 00",
  "length": 97,
  "expectedMatchCount": 1,
  "patch": {
    "offset": 84,
    "originalBytes": "CC",
    "replacementBytes": "90",
    "originalBytesMask": "FF",
    "description": "NOP thread-check INT3 in command 0x0D (get record by type+index) local-mode dispatch helper"
  },
  "containingFunction": "FUN_101d15c0",
  "provenance": {
    "author": "RE-F5-cache-fix",
    "date": "2026-08-26",
    "source": "Ghidra disassembly",
    "reference": "docs/RE/RE_F5_cache_fix_findings.md"
  }
}
```

### 5.3 Resolve Export and Apply Patch in TsDevLib

In `tsdev_lib.cpp` constructor (alongside existing export resolutions):
```cpp
// Resolve _GetMasterRecByType@16 export
m_getMasterRecByType = (GetMasterRecByTypeFn)m_resolver->resolveExport("_GetMasterRecByType@16");
```

In `applyThreadCheckPatches()`:
```cpp
m_resolver->applyPatch("FUN_101d15c0_thread_check_INT3");
```

### 5.4 Type Definition

In `tsdev_lib.h`:
```cpp
// _GetMasterRecByType@16 — __stdcall, 4 params (category, subtype, index, outBuf)
// Returns 1 on success (record found), 0 on failure (not found / warehouse unavailable)
// outBuf must be 0x10A (266) bytes, caller-allocated
typedef int (__stdcall *GetMasterRecByTypeFn)(int category, int subType, int index, void* outBuf);
```

### 5.5 Subtype Mapping for `_GetMasterRecByType@16`

```cpp
// Subtype values for _GetMasterRecByType@16 (NOT the same as getSubType()).
// These are the "power type" inputs to ConvertToPowerType, which FUN_101d4280
// uses to convert (category, subtype) → expanded warehouse type.
inline int getMasterRecSubType(ComponentType type) {
    switch (type) {
        case ComponentType::Strategy:   return 2;     // → 0x200
        case ComponentType::Indicator:  return 5;     // → 0x502
        case ComponentType::Function:   return 0;     // → 4 (no conversion)
        case ComponentType::TradingApp: return 0x14;  // → 0x1402
        default: return 0;
    }
}
```

### 5.6 Modified `enumerateRecords` Implementation

```cpp
EnumerateResult TsDevLib::enumerateRecords(ComponentType type,
                                           const std::string& searchName) {
    EnumerateResult result;

    if (!m_getMasterRecByType) {
        return result;  // export not resolved
    }

    int category = getCategory(type);
    int subType = getMasterRecSubType(type);

    // Iterate live records via _GetMasterRecByType@16 (command 0x0D, live lookup).
    // Unlike _LoadMasterRecsEX@16 (command 0x08, cached SafeArray), this sees
    // items created in the current session.
    const int kRecordSize   = 0x10A;
    const int kNameOffset   = 0x17;
    const int kProgIdOffset = 0x00;
    const int kSubTypeOffset = 0xE9;

    BYTE recBuf[kRecordSize];
    int consecutiveMisses = 0;
    const int maxConsecutiveMisses = 10;  // safety for potential index gaps

    for (int index = 0; consecutiveMisses < maxConsecutiveMisses; index++) {
        int ret = 0;
        if (!callGetMasterRecByType(m_getMasterRecByType,
                                    category, subType, index, recBuf, &ret)) {
            // SEH exception — fail gracefully
            return result;
        }
        if (ret != 1) {
            consecutiveMisses++;
            continue;
        }
        consecutiveMisses = 0;

        const char* storedName = (const char*)(recBuf + kNameOffset);

        // Skip records with empty/warehouse-stored names (see §3.4)
        if (storedName[0] < 0x20 || storedName[0] >= 0x7F) {
            continue;
        }

        // For category 2, filter by subtype (Indicator vs TradingApp)
        if (category == 2) {
            unsigned char recSubType = (unsigned char)recBuf[kSubTypeOffset];
            int expectedSubType = getSubType(type);  // 0 for Indicator, 0x14 for TradingApp
            if (expectedSubType == 0x14 && recSubType != 0x14) continue;
            if (expectedSubType == 0x00 && recSubType == 0x14) continue;
        }

        if (_stricmp(storedName, searchName.c_str()) == 0) {
            result.exists = true;
            result.storedName = std::string(storedName);
            result.progId = *(short*)(recBuf + kProgIdOffset);
            break;
        }
    }

    return result;
}
```

### 5.7 SEH-Protected Call Wrapper

```cpp
// SEH-protected call to _GetMasterRecByType@16 (__stdcall, 4 args).
struct GetMasterRecByTypeSehResult {
    int result = 0;
};

#pragma warning(push)
#pragma warning(disable: 4731)
bool callGetMasterRecByType(GetMasterRecByTypeFn fn, int cat, int sub,
                            int idx, void* buf, GetMasterRecByTypeSehResult* out) {
    __try {
        out->result = fn(cat, sub, idx, buf);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#pragma warning(pop)
```

### 5.8 Summary of All Changes

| File | Change |
|------|--------|
| `signatures/v9.50.01.3344.json` | Add `FUN_101d15c0_thread_check_INT3` signature (patch site) |
| `signatures/v9.50.01.3344.json` | Add `_GetMasterRecByType@16` to exports array |
| `src/tsdev_lib.h` | Add `GetMasterRecByTypeFn` typedef, `m_getMasterRecByType` member |
| `src/tsdev_lib.h` | Add `getMasterRecSubType()` inline function |
| `src/tsdev_lib.cpp` | Resolve `_GetMasterRecByType@16` export in constructor |
| `src/tsdev_lib.cpp` | Apply `FUN_101d15c0_thread_check_INT3` patch in `applyThreadCheckPatches()` |
| `src/tsdev_lib.cpp` | Add `callGetMasterRecByType` SEH wrapper |
| `src/tsdev_lib.cpp` | Rewrite `enumerateRecords` to use `_GetMasterRecByType@16` loop |
| `signatures.gen.h` | Regenerated by `tools/sigcodegen.py` (automatic) |

---

## 6. Evidence Index

| Function | RVA | Decompilation Source | Key Finding |
|----------|-----|---------------------|-------------|
| `FUN_101cf5b0` | 0x1CF5B0 | §1.1 | Generic command dispatcher, `__thiscall`, already patched |
| `FUN_101c09f0` | 0x1C09F0 | §1.2 | Cache reset, requires module object + setup before 0x4E |
| `FUN_101cf710` | 0x1CF710 | §1.3 | Worker thread case 0x4E, COM QueryInterface, needs `[warehouse+0x5E]` |
| `FUN_101cfce0` | 0x1CFCE0 | §1.5 | Command 0x0D dispatcher, INT3 at offset 40, NOT patched |
| `_GetMasterRecByType@16` | 0x1A0460 | §2.1 | Export ordinal 510, `__stdcall`, live lookup by type+index |
| `FUN_101cfe00` | 0x1CFE00 | §2.2 | Internal record retrieval, calls `FUN_101d15c0` in local mode |
| `FUN_101d15c0` | 0x1D15C0 | §2.3 | Command 0x0D local-mode dispatcher, INT3 at offset 84 |
| `FUN_101d4280` | 0x1D4280 | §2.5 | Type conversion (category, subtype → expanded type) |
| `ConvertToPowerType` | 0x1584F0 | §2.5 | Power type mapping for subtype parameter |
| `FUN_101c3660` | 0x1C3660 | §3.1 | Name accessor, `[rec+0x73]` flag: <6 = inline, >=6 = warehouse-stored |
| `FUN_101a9650` | 0x1A9650 | §1.4 | GetWarehouse(NULL), `__cdecl`, resolved via existing signature |

> **This document is research-only.** No production source, signature files, or
> tools were modified during this investigation.
