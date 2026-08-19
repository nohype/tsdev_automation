# RE F5: Cache Refresh Feasibility — Direct cmd 0x4E Dispatch Assessment

## 1. Executive Summary

**Verdict: YES — the automatic cache refresh approach is feasible and remarkably simple.**

After every INSERT, MODIFY, or DELETE operation, `[warehouse+0x5E]` is guaranteed to
be set to a valid IWHouse COM interface pointer. This means we can dispatch cmd 0x4E
(cache refresh) directly on our warehouse with just two memory writes and one function
call — no new patches, no new exports, no inline asm, no module object construction.

The implementation is:
```cpp
void* wh = GetWarehouse(NULL);              // already resolved (m_pGetWarehouse)
*(int*)((char*)wh + 0x12) = 0;              // type code 0 = invalidate ALL caches
FUN_101cf5b0(wh, 0x4E);                     // already INT3-patched at offset 36
```

---

## 2. Background

The cache refresh mechanism (from `RE_F5_frida_trace_analysis.md`):

1. Set `[warehouse+0x12]` = type code (0xC for program-list cache, 0 for ALL)
2. Set `[warehouse+0x5E]` = COM interface pointer (IWHouse)
3. Call `FUN_101cf5b0(warehouse, 0x4E)` — the generic command dispatcher

The worker thread handler for cmd 0x4E (`FUN_101cf710`) does:
```asm
PUSH [EBP + 0x8]        ; type_code from [warehouse+0x12]
MOV  EDI, ECX           ; COM interface from [warehouse+0x5E]
PUSH EDI
MOV  EAX, [EDI]         ; vtable — CRASH if EDI is NULL
CALL [EAX + 0x48]       ; vtable method (cache refresh)
```

**The critical question: is `[warehouse+0x5E]` already set to a valid COM interface
pointer after our insert/modify/delete operations?**

---

## 3. Warehouse Constructor — `[warehouse+0x5E]` Initialized to NULL

**Ghidra evidence** — `FUN_101cf1f0` (warehouse constructor, RVA 0x1CF1F0):

```c
undefined4 * __fastcall FUN_101cf1f0(undefined4 *param_1)
{
    *param_1 = CELWHouse::vftable;
    // ... many field initializations ...
    *(undefined4 *)((int)param_1 + 0x5e) = 0;   // ← [warehouse+0x5E] = NULL
    // ... more initializations ...
    return param_1;
}
```

`GetWarehouse(NULL)` (`FUN_101a9650`) creates the warehouse via
`operator_new(0xea)` + `FUN_101cf1f0()` (constructor) + `FUN_101a92a0()` (initializer).

**Conclusion**: `[warehouse+0x5E]` is initialized to **0 (NULL)** during warehouse
creation. It must be set by a subsequent command dispatch.

---

## 4. Switch Case Analysis — Commands That SET `[warehouse+0x5E]`

The worker thread handler `FUN_101d2150` (RVA 0x1D2150) contains a large switch
statement processing warehouse commands. In the Ghidra decompilation, `param_1` is
typed as `IUnknownVtbl *` (12 bytes per element), so `[warehouse+0x5E]` (offset 94)
appears as `param_1[7].Release + 2` (7×12 + 8 + 2 = 94 = 0x5E).

### 4.1 Commands That WRITE `[warehouse+0x5E]` (SET the COM interface)

All five "lookup/load" commands follow the same pattern:

```c
// Example: case 0x10 (ProgramByNameTypes)
piVar9 = *(int **)((int)&param_1[0xe].AddRef + 2);   // [warehouse+0xAE] = COM server
iVar8 = (**(code **)(*piVar9 + 0xac))(piVar9, ...);   // vtable call
*(int *)((int)&param_1[6].AddRef + 2) = iVar8;       // [warehouse+0x76] = result
if (iVar8 != 0) goto LAB_101d285d;                    // failure → [warehouse+0x5E] = 0
// success path:
FUN_101cf020();                                        // COM error check + get interface
*(int *)((int)&param_1[7].Release + 2) = local_3c;   // [warehouse+0x5E] = COM interface
```

| Command | Case | Vtable Method | Sets `[warehouse+0x5E]` on success | Sets to 0 on failure |
|---------|------|---------------|--------------------------------------|----------------------|
| 0x09 | `case 0x09` | 0x24 | YES | YES (via `LAB_101d285d`) |
| 0x0D | `case 0x0d` | 0x20 | YES | YES (via `LAB_101d285d`) |
| 0x0E | `case 0x0e` | 0x3C | YES | YES (via `LAB_101d285d`) |
| 0x0F | `case 0x0f` | 0xAC | YES | YES (via `LAB_101d285d`) |
| 0x10 | `case 0x10` | 0xAC | YES | YES (via `LAB_101d285d`) |

`LAB_101d285d` is the shared failure path:
```c
LAB_101d285d:
    *(undefined4 *)((int)&param_1[7].Release + 2) = 0;   // [warehouse+0x5E] = 0
```

### 4.2 Commands That READ `[warehouse+0x5E]` (USE the COM interface)

These commands call vtable methods on the COM interface from `[warehouse+0x5E]`:

| Command | Case | Vtable Method | Action |
|---------|------|---------------|--------|
| 0x33 | `case 0x33` | 0x2C | Get name |
| 0x35 | `case 0x35` | 0x24 | LazyInit |
| 0x4F | `case 0x4f` | 0x08 | Release |
| 0x50 | `case 0x50` | 0x04 | AddRef |
| 0x51 | `case 0x51` | 0x90 | Get property |
| 0x52 | `case 0x52` | 0x94 | Set property |

### 4.3 Commands That DON'T Touch `[warehouse+0x5E]`

| Command | Case | Action |
|---------|------|--------|
| 0x17 | `case 0x17` | Calls `FUN_101d2000` — no `[warehouse+0x5E]` access |

---

## 5. Operation Flow Analysis — Is `[warehouse+0x5E]` Set After Each Operation?

### 5.1 INSERT Flow

```
findExistPos(name) → createEntry(type, name) → saveSource(type, progId, source) → verifyModule(type, progId)
```

#### 5.1.1 findExistPos (cmd 0x10 via FUN_101d18c0 — BST cache miss only)

`_FindExistPos` (RVA 0x1A4230) calls `FUN_101d05e0` which checks server mode:

```c
// FUN_101d05e0 (RVA 0x1D05E0)
if (*(int *)(param_1 + 0xc2) == 1) {   // server mode
    FUN_101a9460(param_2, param_3);
    iVar1 = FUN_101e3db0(param_2, param_3);   // BST lookup
    *param_4 = iVar1;
    return *param_4 != 0;
}
FUN_101d18c0(param_2, param_3, param_4);      // local mode
```

In server mode, `FUN_101e3db0` (RVA 0x1E3DB0) searches the BST cache. On BST cache
**miss**, it falls back to `FUN_101d18c0`:

```c
// FUN_101e3db0 — BST miss fallback
if (local_14 == 0) {
    LeaveCriticalSection(...);
    FUN_101a9650();                     // GetWarehouse(NULL)
    iVar6 = FUN_101d18c0();             // ← dispatches cmd 0x10!
    if (iVar6 == 1) {
        FUN_101c4990();
        FUN_101e30f0();
        // ... post-processing ...
    }
}
```

`FUN_101d18c0` (RVA 0x1D18C0) dispatches **cmd 0x10**:
```c
*(undefined4 *)(param_1 + 4) = 0x10;   // dispatch cmd 0x10
```

**Summary for findExistPos**:
- BST cache **hit** (item exists at init time): cmd 0x10 NOT dispatched → `[warehouse+0x5E]` NOT set
- BST cache **miss** (item not in cache): cmd 0x10 dispatched → `[warehouse+0x5E]` SET on success, 0 on failure
- For INSERT (item doesn't exist): BST miss → cmd 0x10 dispatched → if vtable returns non-zero (not found), `[warehouse+0x5E] = 0`

#### 5.1.2 createEntry (cmd 0x09 via FUN_101d0860 + cmd 0x0E via FUN_101d3c00)

`_CreateEntryEX@24` (RVA 0x159610) calls `FUN_101cf990` (RVA 0x1CF990):

```c
// FUN_101cf990 — dispatches cmd 0x16, then calls FUN_101d0860
*(undefined4 *)(param_1 + 0x12) = param_2;  // [warehouse+0x12] = type
*(undefined4 *)(param_1 + 8) = param_3;     // [warehouse+0x08] = name
*(undefined4 *)(param_1 + 4) = 0x16;        // dispatch cmd 0x16
// ... wait for completion ...
if (*(int *)(param_1 + 0x4e) == 0) {
    FUN_101d0860(uVar4, uVar2, &param_2);   // ← post-processing
    return param_2;
}
```

`FUN_101d0860` (RVA 0x1D0860) dispatches **cmd 0x09**:
```c
// FUN_101d0860
*(undefined4 *)(param_1 + 0x12) = param_2;  // [warehouse+0x12] = type
*(undefined4 *)(param_1 + 8) = param_3;     // [warehouse+0x08] = name
*(undefined4 *)(param_1 + 4) = 9;           // dispatch cmd 0x09
// ... wait for completion ...
if (*(int *)(param_1 + 0x5e) != 0) {        // check [warehouse+0x5E]
    pvVar4 = operator_new(0x275);            // allocate module object
    iVar5 = FUN_1000dd30();                  // init CELProgramWriter
    FUN_101c4ab0(param_1, *(undefined4 *)(param_1 + 0x5e));  // set [module+0x10] = COM interface
    return 1;
}
```

**Cmd 0x09 SETS `[warehouse+0x5E]`** on success (see §4.1).

Then `_CreateEntryEX@24` calls `FUN_101d3c00` (RVA 0x1D3C00):

```c
// FUN_101d3c00 — dispatches cmd 0x0E
*(undefined4 *)(param_1 + 0x12) = pvVar3;   // [warehouse+0x12] = type
*(int *)(param_1 + 0x16) = iVar4;           // [warehouse+0x16] = progId
*(undefined4 *)(param_1 + 4) = 0xe;         // dispatch cmd 0x0E
// ... wait for completion ...
return (uint)(*(int *)(param_1 + 0x4e) == 0);  // return 1 on success
```

**Cmd 0x0E SETS `[warehouse+0x5E]`** on success (see §4.1).

**After createEntry: `[warehouse+0x5E]` IS SET** ✓ (by both cmd 0x09 and cmd 0x0E)

#### 5.1.3 saveSource (cmd 0x0E via FUN_101d3c00)

`_SaveModule_New@20` (RVA 0x159A70) → `FUN_10159770` (RVA 0x159770) → `FUN_101a07f0` (RVA 0x1A07F0):

```c
// FUN_101a07f0
iVar1 = FUN_101a9650(0);                    // GetWarehouse(NULL)
uVar2 = FUN_101d4280(param_1, ...);         // expand type
if (param_3 == 0) {
    iVar1 = FUN_101d3c00(uVar2, param_2, param_4);  // ← dispatches cmd 0x0E!
} else {
    iVar1 = FUN_101d3d00(uVar2, param_3, param_4);
}
if (iVar1 == 0) {                           // first save failed
    puVar4 = FUN_101cf990(uVar2, uVar3);    // fallback: cmd 0x16 + cmd 0x09
    // ... retry save ...
}
return 1;
```

`FUN_101d3c00` dispatches **cmd 0x0E** which SETS `[warehouse+0x5E]` on success.

**After saveSource: `[warehouse+0x5E]` IS SET** ✓ (by cmd 0x0E via FUN_101d3c00)

#### 5.1.4 verifyModule (wrong warehouse — does NOT affect our warehouse)

`_VerifyModule_New@20` calls `FUN_101a8620` with the 50-space string path, which
creates/uses a DIFFERENT warehouse. Our warehouse's `[warehouse+0x5E]` is unaffected.

#### 5.1.5 INSERT Summary

| Step | Command | Sets `[warehouse+0x5E]`? |
|------|---------|--------------------------|
| findExistPos | cmd 0x10 (BST miss only) | Maybe (0 on failure) |
| createEntry | cmd 0x09 + cmd 0x0E | **YES** ✓ |
| saveSource | cmd 0x0E | **YES** ✓ |
| verifyModule | cmd 0x0F (wrong warehouse) | No (different warehouse) |

**After INSERT: `[warehouse+0x5E]` IS SET** ✓

---

### 5.2 MODIFY Flow

```
findExistPos(name) → saveSource(type, progId, source) → verifyModule(type, progId)
```

| Step | Command | Sets `[warehouse+0x5E]`? |
|------|---------|--------------------------|
| findExistPos | cmd 0x10 (BST hit — item exists at init) | No (BST cache hit, cmd 0x10 not dispatched) |
| saveSource | cmd 0x0E via FUN_101d3c00 | **YES** ✓ |
| verifyModule | cmd 0x0F (wrong warehouse) | No (different warehouse) |

**After MODIFY: `[warehouse+0x5E]` IS SET** ✓ (by cmd 0x0E during saveSource)

---

### 5.3 DELETE Flow

```
findExistPos(name) → deleteEntry(type, name)
```

`deleteEntry` calls `FUN_101a8360` (delete orchestrator, RVA 0x1A8360):

```c
// FUN_101a8360 — __fastcall: ECX=whType, EDX=progId
local_20 = FUN_101a9650(0);                    // GetWarehouse(NULL) → OUR warehouse!
FUN_101d43a0(param_1, local_74);               // expand types
FUN_1000dd30();                                 // init CELProgramWriter
iVar3 = FUN_101d0950(local_74, param_2, local_2ec);  // cmd 0x0F → SETS [warehouse+0x5E]!
if (iVar3 == 1) {
    // ... process module elements ...
    FUN_101a84e0(1);                            // actual delete
    if ((param_1 == 0) || (param_1 == 1)) {     // category 0 or 1
        FUN_101c09f0(0xb);                      // cache reset type 0xB
        FUN_101c09f0(0xc);                      // cache reset type 0xC
    }
    FUN_101d1fa0(uVar5, uVar4);                 // cmd 0x17 (SAVE/COMMIT)
}
```

`FUN_101d0950` (RVA 0x1D0950) dispatches **cmd 0x0F**:
```c
// FUN_101d0950
*(undefined4 *)(param_1 + 4) = 0xf;           // dispatch cmd 0x0F
// ... wait for completion ...
if (*(int *)(param_1 + 0x4e) == 0) {           // success
    FUN_101c4ab0(param_1, *(undefined4 *)(param_1 + 0x5e));  // use [warehouse+0x5E]
    return 1;
}
```

**Cmd 0x0F SETS `[warehouse+0x5E]`** on success (see §4.1).

| Step | Command | Sets `[warehouse+0x5E]`? |
|------|---------|--------------------------|
| findExistPos | cmd 0x10 (BST hit) | No |
| deleteEntry | cmd 0x0F via FUN_101d0950 | **YES** ✓ |

**After DELETE: `[warehouse+0x5E]` IS SET** ✓ (by cmd 0x0F during deleteEntry)

**Additional note**: The delete orchestrator already calls `FUN_101c09f0(0xB)` and
`FUN_101c09f0(0xC)` for category 0/1, which dispatch cmd 0x4E (cache refresh) on our
warehouse. For other categories (2, 4, TradingApp), the cache is NOT refreshed by the
delete orchestrator — our cache refresh would fill this gap.

---

### 5.4 cmd 0x35 (LazyInit) — Does NOT Set `[warehouse+0x5E]`

Case 0x35 in the switch:
```c
case (_func_5022 *)0x35:
    piVar9 = *(int **)((int)&param_1[7].Release + 2);   // READ [warehouse+0x5E]
    uVar11 = (**(code **)(*piVar9 + 0x24))(piVar9);      // call vtable method 0x24
    *(undefined4 *)((int)&param_1[6].AddRef + 2) = uVar11;
    break;
```

**cmd 0x35 READS `[warehouse+0x5E]` but does NOT write it.** It uses the existing
COM interface; it does not create or update it.

---

## 6. Type Code Analysis (Task 5)

### 6.1 Type Codes and Cache Flags

From `FUN_101c09f0` decompilation (RVA 0x1C09F0):

| Type Code | Cache Offset | Purpose |
|-----------|-------------|---------|
| 0 | ALL | Invalidate all cache flags |
| 6 | `[module+0x8F]` | Specific cache |
| 9 (`0x9`) | `[module+0x1C1]` | Specific cache |
| 10 (`0xA`) | `[module+0x1B8]` | Specific cache |
| 11 (`0xB`) | `[module+0x1C9]` | Specific cache |
| **12 (`0xC`)** | **`[module+0x1DE]`** | **Program-list cache (used by cmd 0x08)** |
| 13 (`0xD`) | `[module+0x1FC]` | Specific cache |
| 17 (`0x11`) | `[module+0x22D]` | Specific cache |

### 6.2 Type Codes Used by Existing Callers

| Caller | Type Codes | Context |
|--------|-----------|---------|
| `FUN_101a8620` (save/compile orchestrator) | `0xC` only | After saving/verifying non-Function items |
| `FUN_101a8360` (delete orchestrator) | `0xB`, `0xC` | After deleting category 0/1 items only |
| `FUN_1016f380` (full save flow) | `0xA`, `0x9`, `0xC`, `0x11` | Full cache reset after save |

**Ghidra evidence** for `FUN_1016f380` (RVA 0x16F380):
```c
FUN_101c09f0(10);      // type code 0xA
FUN_101c09f0(9);       // type code 0x9
FUN_101c09f0(0xc);     // type code 0xC
FUN_101c09f0(0x11);    // type code 0x11
```

**Ghidra evidence** for `FUN_101a8360` (RVA 0x1A8360):
```c
if ((param_1 == 0) || (param_1 == 1)) {
    FUN_101c09f0(0xb);
    FUN_101c09f0(0xc);
}
```

### 6.3 Recommendation: Use Type Code 0 (ALL)

**Rationale**:

1. **Simplicity**: One dispatch instead of multiple. No need to map component types
   to specific type codes.

2. **Safety**: `FUN_101a8620` uses only `0xC` for non-Function types, but
   `FUN_101a8360` uses `0xB` AND `0xC` for category 0/1, and `FUN_1016f380` uses
   four different codes. This suggests different caches may need invalidation for
   different operations and component types. Type code 0 covers ALL caches,
   eliminating the risk of missing a cache.

3. **Correctness for all component types**: We support Strategy (0), Indicator (2),
   Function (4), and TradingApp (5122). `FUN_101a8620` only uses `0xC` for non-
   Function types — we have no evidence that `0xC` alone is sufficient for Function
   (category 4) or TradingApp. Type code 0 eliminates this uncertainty.

4. **Performance**: The cache refresh is a server-side COM call (IWHouse vtable
   method 0x48). Refreshing all caches vs. one cache is a negligible difference for
   a single refresh after each operation.

5. **Precedent**: `FUN_101c09f0` explicitly supports type code 0 as "invalidate ALL
   cache flags" — this is a designed feature, not a hack.

**Alternative**: If type code 0 has unexpected side effects, fall back to `0xC`
(the program-list cache used by cmd 0x08). This is what `FUN_101a8620` uses for
all non-Function types and is the most targeted option.

---

## 7. Simplest Possible Cache Refresh Implementation (Task 4)

### 7.1 The Implementation

Since `[warehouse+0x5E]` IS set after every INSERT/MODIFY/DELETE operation, the
cache refresh is simply:

```cpp
// 1. Get warehouse pointer (already resolved: m_pGetWarehouse)
void* wh = nullptr;
callGetWarehouse(m_pGetWarehouse, 0, &wh);  // GetWarehouse(NULL)
if (!wh) return false;

// 2. Set type code (0 = invalidate ALL caches)
*(int*)((char*)wh + 0x12) = 0;

// 3. Dispatch cmd 0x4E (FUN_101cf5b0 is already INT3-patched at offset 36)
//    FUN_101cf5b0 is __thiscall: ECX = warehouse, stack = command ID
//    Need to call it with ECX = wh and stack arg = 0x4E
```

### 7.2 How to Call FUN_101cf5b0

`FUN_101cf5b0` is `__thiscall`: ECX = warehouse (this), stack = command ID.

The function pointer can be obtained from the existing patch site resolution:
- The `FUN_101cf5b0_thread_check_INT3` signature starts at the function entry point
- `PatchSiteDescriptor::sigMatchRva` gives the function's RVA
- Adding the base address gives the function pointer

**Option A: Typedef (preferred — simplest)**:
```cpp
typedef void (__thiscall *CmdDispatchFn)(void* warehouse, int cmdId);
CmdDispatchFn fn = (CmdDispatchFn)(baseAddress + sigMatchRva);
fn(wh, 0x4E);
```

MSVC `__thiscall` uses ECX for `this` and stack for remaining params, with
callee cleanup (`RET 4` for one stack param). This matches `FUN_101cf5b0`'s
convention exactly (confirmed: the function ends with `RET 4`).

**Option B: Inline asm (if __thiscall typedef has issues)**:
```cpp
#pragma warning(push)
#pragma warning(disable: 4731)
void callCmdDispatch(void* fnPtr, void* warehouse, int cmdId) {
    __asm {
        push cmdId
        mov ecx, warehouse
        call fnPtr
    }
}
#pragma warning(pop)
```

### 7.3 What's Already Available

| Requirement | Status |
|-------------|--------|
| Warehouse pointer | `m_pGetWarehouse` — already resolved ✓ |
| `FUN_101cf5b0` address | `resolveThreadCheck2().sigMatchRva` — already resolved ✓ |
| `FUN_101cf5b0` INT3 patch | Already applied at offset 36 ✓ |
| `[warehouse+0x5E]` set | Set by cmd 0x09/0x0E/0x0F during our operations ✓ |
| `[warehouse+0x12]` write | Simple memory write ✓ |

**No new patches, no new exports, no new signature scans needed.**

---

## 8. Safety Assessment

### 8.1 What cmd 0x4E Does

The worker thread handler for cmd 0x4E calls IWHouse vtable method 0x48 on the COM
interface, passing the type code. This is a **non-destructive server-side cache
refresh** — it tells the WHServer to refresh its cache for the specified type. It
does not modify warehouse data.

### 8.2 Thread Safety

`FUN_101cf5b0` uses:
1. `WaitForSingleObject([warehouse+0x72], INFINITE)` — acquires mutex
2. Thread check (INT3 at offset 36 — already patched to NOP)
3. `SetEvent([warehouse+0x6e])` — signals worker thread
4. `WaitForSingleObject([warehouse+0x6a], INFINITE)` — waits for completion
5. `ReleaseMutex([warehouse+0x72])` — releases mutex

This is thread-safe: the mutex serializes access, and the event pair synchronizes
with the worker thread. The INT3 patch allows our thread to dispatch commands.

### 8.3 What Happens If `[warehouse+0x5E]` is NULL

If `[warehouse+0x5E]` is 0 (NULL), the cmd 0x4E handler (`FUN_101cf710`) will crash:
```asm
MOV  EDI, ECX           ; EDI = 0 (NULL)
MOV  EAX, [EDI]         ; ← ACCESS VIOLATION (dereference NULL)
```

**Mitigation**: Check `[warehouse+0x5E]` before dispatching:
```cpp
void* comInterface = *(void**)((char*)wh + 0x5E);
if (!comInterface) return false;  // safety check
```

However, based on our analysis, `[warehouse+0x5E]` is ALWAYS set after our
operations (INSERT/MODIFY/DELETE). The safety check is a defensive measure.

### 8.4 SEH Protection

The call should be SEH-protected (like all other orlib20 calls in our CLI):
```cpp
__try {
    fn(wh, 0x4E);
    return true;
} __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
}
```

### 8.5 When Not to Refresh

- If the operation failed (e.g., createEntry returned ProgId ≤ 0), do NOT refresh.
  The warehouse state hasn't changed.
- If `GetWarehouse(NULL)` returns NULL, do NOT refresh.

---

## 9. Complexity Assessment

| Aspect | Complexity |
|--------|------------|
| New patches needed | **0** (FUN_101cf5b0 already patched) |
| New signature scans needed | **0** (function address from existing patch resolution) |
| New exports needed | **0** |
| Inline asm needed | **0** (can use __thiscall typedef) |
| Module object construction | **0** (not needed) |
| Memory writes | **1** (`[warehouse+0x12] = type_code`) |
| Function calls | **2** (GetWarehouse + FUN_101cf5b0) |
| Total lines of new code | **~10-15** |

**Complexity: VERY LOW.** This is dramatically simpler than all previously
evaluated approaches (C, D, E, F from `RE_F5_approach_evaluation.md`).

---

## 10. Comparison with Previously Evaluated Approaches

| Approach | New Patches | New Sigs | Inline ASM | Module Object | Complexity |
|----------|-------------|----------|------------|---------------|------------|
| **C: Direct FUN_101c09f0** | 0 | 1 | No | Yes | HIGH |
| **D: Direct cmd 0x4E + manual setup** | 1 | 1 | Maybe | No | MEDIUM-HIGH |
| **E: Close + reinit warehouse** | 0 | 0 | No | No | LOW (but HIGH risk) |
| **F: FUN_101a8620 with NULL path** | 0 | 1 | Yes | No | MEDIUM |
| **G: Hybrid (live lookup)** | 1 | 1 | No | No | MEDIUM |
| **★ Direct cmd 0x4E (this approach)** | **0** | **0** | **No** | **No** | **VERY LOW** |

---

## 11. Verdict

**The automatic cache refresh approach (direct cmd 0x4E dispatch) is FEASIBLE and
is the SIMPLEST of all evaluated approaches.**

### Key findings:

1. **`[warehouse+0x5E]` is set after ALL our operations** (INSERT, MODIFY, DELETE):
   - INSERT: cmd 0x09 (via `FUN_101d0860`) + cmd 0x0E (via `FUN_101d3c00`) during createEntry
   - MODIFY: cmd 0x0E (via `FUN_101d3c00`) during saveSource
   - DELETE: cmd 0x0F (via `FUN_101d0950`) during deleteEntry

2. **`[warehouse+0x5E]` is NOT set during warehouse initialization** — it's NULL from
   the constructor. It's set by the first "lookup/load" command dispatch.

3. **cmd 0x35 (LazyInit) does NOT set `[warehouse+0x5E]`** — it only reads it.

4. **Type code 0 (ALL caches) is recommended** for maximum safety across all
   component types. Type code 0xC is a viable alternative for targeted refresh.

5. **No new patches, signatures, or exports are needed.** The function address and
   INT3 patch for `FUN_101cf5b0` are already resolved and applied.

6. **The implementation is ~10-15 lines of code**: get warehouse pointer, set type
   code, call `FUN_101cf5b0(warehouse, 0x4E)`.

### Recommendation:

This approach should be the **recommended approach** for TVS-F.5 (case-insensitive
`exists` via enumeration). It is simpler, safer, and more complete than the Hybrid
approach (Approach G) previously recommended in `RE_F5_approach_evaluation.md`:

- **Simpler**: ~15 lines vs. Hybrid's new patch + new export + enumeration logic
- **More complete**: Refreshes the cached `_LoadMasterRecsEX@16` to see ALL current
  items (including items created by other processes after our init), not just
  session-created items
- **No new patches**: Reuses the already-applied `FUN_101cf5b0` INT3 patch
- **No new exports**: Uses the already-resolved `GetWarehouse` function pointer

### Caveat:

The cache refresh makes the cached cmd 0x08 enumeration see current state. However,
the cmd 0x4E dispatch is a **server-side** refresh — it tells the WHServer to
refresh its cache. The next cmd 0x08 call will then re-fetch the data from the
server. This means:
- Items created in the current session WILL be visible
- Items deleted in the current session WILL NOT be visible
- Items created by other processes WILL be visible (server has current state)

This is the desired behavior for the `exists` use case.
