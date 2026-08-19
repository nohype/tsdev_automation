# RE F5: Frida Trace Analysis — Cache-Reset Caller, verifyModule Flow, and Approach Evaluation

## 1. Frida Trace Summary

A Frida trace of TSDev's Open dialog flow, captured after `_CreateEntryEX@24` created a
new warehouse entry (ProgId `5005`), showed the following sequence:

1. **Entry creation** returns ProgId `5005`.
2. **Post-creation commands** run (save/compile path).
3. **`FUN_101c09f0` is called with type code `12` (`0xC`)** — the cache-reset function.
4. **Command `0x4E` is dispatched** through `FUN_101cf5b0`, with `[warehouse+0x12] = 0xC`.
5. **`_GetMasterRecByType@16`** (live lookup, cmd 0x0D) confirms the new item is visible.
6. **Later command `0x08` enumeration** (cached SafeArray) sees the new item.

The trace proves that the cache-reset sequence (`FUN_101c09f0(0xC)` → cmd `0x4E`) makes
newly created items visible to the cached cmd `0x08` enumeration path.

---

## 2. Ghidra Analysis of the Cache-Reset Caller

### 2.1 `FUN_101a8620` — Save/Compile Orchestrator (RVA `0x1A8620`)

The cache-reset call at RVA `0x1A8716` resides inside `FUN_101a8620`, a `__fastcall`
orchestrator with three parameters:

- **ECX (`param_1`)**: warehouse type (whType / category)
- **EDX (`param_2`)**: ProgId
- **Stack (`param_3`)**: path string (wide pointer, passed to `GetWarehouse`)

Decompilation (Ghidra, abridged):

```c
void __fastcall FUN_101a8620(undefined4 param_1, undefined4 param_2, undefined4 param_3)
{
    if ((DAT_1023aaac != 1) &&
        (iVar2 = FUN_101a9650(param_3), iVar2 != 0))   // GetWarehouse(param_3)
    {
        FUN_1000dd30();                                   // init CELProgramWriter
        FUN_101d43a0(param_1, local_5c);                  // expand types
        iVar2 = FUN_101d0950(local_5c, param_2, local_2d4); // cmd 0x0F (load program)
        if (iVar2 != 0) {
            // ... process module elements in a loop ...
            FUN_101c09f0(0xc);                             // ← CACHE RESET (type code 0xC)
        }
        // ... cleanup (CELProgramWriter dtor, etc.) ...
    }
}
```

Key assembly at the reset site:

```asm
101a8709  PUSH 0xc                    ; type code 0xC
101a870b  LEA  ECX, [EBP-0x2d0]       ; this = module/context object (local)
101a8711  CALL 0x101c09f0             ; FUN_101c09f0(module, 0xC)
```

The module/context object at `[EBP-0x2D0]` is a `CELProgramWriter` initialized by
`FUN_1000dd30` and populated by `FUN_101d0950` (cmd 0x0F).

### 2.2 `FUN_101c09f0` — Cache Invalidation + Command 0x4E Dispatch (RVA `0x1C09F0`)

`__thiscall`: ECX = module object, stack = type code.

Decompilation (abridged):

```c
void __thiscall FUN_101c09f0(int param_1, int param_2)
{
    // Phase 1: Invalidate caches based on type_code
    //   param_2 == 0  → invalidate ALL cache flags
    //   param_2 == 6  → [module+0x8F]
    //   param_2 == 9  → [module+0x1C1]
    //   param_2 == 10 → [module+0x1B8]
    //   param_2 == 0xB → [module+0x1C9]
    //   param_2 == 0xC → [module+0x1DE]
    //   param_2 == 0xD → [module+0x1FC]
    //   param_2 == 0x11 → [module+0x22D]

    // Phase 2: Lock sync object at [module+0x218]
    CSingleLock::CSingleLock(local_34, *(CSyncObject **)(param_1 + 0x218), 1);
    FUN_101c82b0();

    // Phase 3: Resolve COM interface if [module+0x10] == 0
    if (*(int *)(param_1 + 0x10) == 0) {
        iVar3 = FUN_101cfce0(  // cmd 0x0D — UNPATCHED INT3 at offset 40
            *(short *)(param_1 + 0x15),    // ProgId
            *(undefined4 *)(param_1 + 0x18) // value
        );
        *(int *)(param_1 + 0x10) = iVar3;
        if (iVar3 == 0) { swi(3); }  // INT3 crash if resolution fails
    }

    // Phase 4: Set warehouse fields and dispatch 0x4E
    //   warehouse = [module+0xC]
    *(undefined4 *)(*(int *)(param_1 + 0xc) + 0x5e) = *(undefined4 *)(param_1 + 0x10);
    //   → [warehouse+0x5E] = [module+0x10]  (COM interface pointer)

    *(int *)(*(int *)(param_1 + 0xc) + 0x12) = param_2;
    //   → [warehouse+0x12] = type_code

    FUN_101cf5b0(0x4e);  // dispatch cmd 0x4E on warehouse (ECX = [module+0xC])

    CSingleLock::Unlock(local_34);
}
```

**Critical detail**: `FUN_101cfce0` (cmd 0x0D) has an **unpatched INT3 at offset 40**
(`0x101CFD08`). It is only called when `[module+0x10] == 0` (COM interface not cached).
In the normal save/compile flow, `FUN_101d0950` (cmd 0x0F) populates `[module+0x10]`
via `FUN_101c4ab0`, so `FUN_101cfce0` is **skipped**.

### 2.3 `FUN_101cf710` — Worker Thread Case 0x4E (RVA `0x1CF710`)

Disassembly:

```asm
101cf715  PUSH [EBP + 0x8]        ; param_2 = type_code (from [warehouse+0x12])
101cf718  MOV  EDI, ECX           ; param_1 = COM interface (from [warehouse+0x5E])
101cf71a  PUSH EDI
101cf71b  MOV  EAX, [EDI]         ; vtable
101cf71d  CALL [EAX + 0x48]       ; vtable method at offset 0x48
```

This calls **IWHouse vtable offset 0x48** (oVft 0x90) on the COM interface, passing the
type code. This is NOT `FlushCache` (vtable 0x80, cmd 0x45) — it is a distinct
cache-refresh method that refreshes the server-side cache for the specified type,
making new items visible on the next cmd 0x08 call.

### 2.4 Worker Thread Handler — Command 0x4E Case (in `FUN_101d2150`)

At `0x101D3333`:

```asm
101d3333  PUSH [EDI + 0x12]       ; type_code from [warehouse+0x12]
101d3336  MOV  ECX, [EDI + 0x5E]  ; COM interface from [warehouse+0x5E]
101d3339  CALL 0x101cf710         ; FUN_101cf710(COM_interface, type_code)
101d333e  JMP  0x101d2ecc         ; → back to dispatch loop
```

The handler reads `[warehouse+0x12]` (type code) and `[warehouse+0x5E]` (COM interface)
— exactly the fields set by `FUN_101c09f0` before dispatching cmd 0x4E.

---

## 3. Module-Object Acquisition

### 3.1 How `FUN_101a8620` Obtains the Module Object

1. **`FUN_1000dd30()`** — initializes a local `CELProgramWriter` object (the module
   context). This is a stack-allocated object at `[EBP-0x2D0]`.

2. **`FUN_101d43a0(param_1, local_5c)`** — expands the category into an array of
   warehouse types (e.g., category 0 → `{0, 0x200, -1}`).

3. **`FUN_101d0950(local_5c, param_2, local_2d4)`** — dispatches cmd 0x0F
   (`IWHouse::ProgramByIdTypes`) on the warehouse. On success, calls
   `FUN_101c4ab0(module, warehouse, COM_result)` which sets:
   - `[module+0x0C]` = warehouse pointer
   - `[module+0x10]` = COM interface pointer (from `[warehouse+0x5E]`, the cmd 0x0F result)

4. **`FUN_101a8750`** — obtains an additional module/program object via
   `FUN_101d0590` (cmd 0x0F or server-mode equivalent) and `FUN_101c2710`
   (initializes a `CELPrograms` context).

### 3.2 `FUN_101c4ab0` — Module Field Setter (RVA `0x1C4AB0`)

```c
void __thiscall FUN_101c4ab0(int *param_1, int param_2, int param_3)
{
    // vtable calls (cleanup of previous state)
    (**(code **)(*param_1 + 4))();
    (**(code **)(*param_1 + 0xc))();
    param_1[3] = param_2;   // [module+0x0C] = warehouse
    param_1[4] = param_3;   // [module+0x10] = COM interface
}
```

This confirms that `FUN_101d0950` sets `[module+0x10]` to the COM interface result from
cmd 0x0F. Therefore, in the normal flow, `[module+0x10] != 0` when `FUN_101c09f0` is
called, and `FUN_101cfce0` (unpatched INT3) is skipped.

---

## 4. Type-Code Values

### 4.1 Cache-Reset Type Codes (param_2 of `FUN_101c09f0`)

| Type Code | Cache Offset | Purpose |
|-----------|-------------|---------|
| 0 | ALL | Invalidate all cache flags |
| 6 | `[module+0x8F]` | Specific cache |
| 9 | `[module+0x1C1]` | Specific cache |
| 10 (`0xA`) | `[module+0x1B8]` | Specific cache |
| 11 (`0xB`) | `[module+0x1C9]` | Specific cache |
| **12 (`0xC`)** | **`[module+0x1DE]`** | **Program-list cache (used by cmd 0x08)** |
| 13 (`0xD`) | `[module+0x1FC]` | Specific cache |
| 17 (`0x11`) | `[module+0x22D]` | Specific cache |

### 4.2 Type Codes Used by Callers of `FUN_101c09f0`

| Caller | Type Codes | Context |
|--------|-----------|---------|
| `FUN_101a8620` (save/compile orchestrator) | `0xC` | After saving/verifying non-Function items |
| `FUN_101a8360` (delete orchestrator) | `0xB`, `0xC` | After deleting category 0/1 items |
| `FUN_1016f380` (full save flow) | `0xA`, `0x9`, `0xC`, `0x11` | Full cache reset after save |

### 4.3 Component-Type to Category/Subtype Mapping

From `src/tsdev_lib.h`:

| ComponentType | Expanded whType | Category | Subtype |
|---------------|----------------|----------|---------|
| Strategy | 0 | 0 | 0 |
| Indicator | 2 | 2 | 0 |
| Function | 4 | 4 | 0 |
| TradingApp | 5122 (`0x1402`) | 2 | `0x14` |

The adversarial review (`RE_F5_cache_fix_review.md` Finding 1) confirmed that
`getSubType(type)` (which returns `type >> 8`) gives the correct subtype values for
`_GetMasterRecByType@16`: Strategy=0, Indicator=0, Function=0, TradingApp=0x14.
The previously proposed values (Strategy=2, Indicator=5) were **incorrect** and would
miss all user-created Strategy and Indicator items.

---

## 5. Does `verifyModule` Trigger the Cache Reset?

### 5.1 `_VerifyModule_New@20` Calls `FUN_101a8620` for Non-Function Types

Decompilation of `_VerifyModule_New@20` (RVA `0x169630`) confirms:

```c
if (param_2 != 4) {   // skip for Function (category 4)
    local_2ac = local_2a8;
    FUN_10009eb0(s__10230710, 3);   // convert 50-space string to wide
    FUN_101a8620(local_2ac);         // ← calls FUN_101a8620 with 50-space path
    // ...
}
```

All three callers of `FUN_101a8620` (`_VerifyModule_New@20`, `FUN_10159ae0`,
`FUN_10168ee0`) use the same hardcoded string `s__10230710` — a 50-byte string of
space characters (`0x20`) at VA `0x10230710`.

### 5.2 CRITICAL FINDING: Path Mismatch — Cache Reset Operates on Wrong Warehouse

`FUN_101a8620` calls `FUN_101a9650(param_3)` (GetWarehouse) with the 50-space string
as the path parameter. **This does not match our CLI's warehouse.**

**How warehouse path matching works** (from `FUN_101a9650` decompilation):

- When `param_1 == 0` (NULL): uses `DAT_102408fc` (the CSIDL_PROGRAM_FILES_COMMON
  path, set by `OrlibInit`) as the search key.
- When `param_1 != 0` (non-NULL): converts the wide string to ANSI and uses it as
  the search key.
- Searches the existing warehouse list via `lstrcmpiA(warehouse_path, search_key)`.
- If no match found, **creates a new warehouse** with the given path.

**Our CLI's warehouse** is created by the first `GetWarehouse(NULL)` call (e.g., from
`findExistPos` or `_LoadMasterRecsEX@16`), registered under the CSIDL path.

**The 50-space path** does not match the CSIDL path. Therefore:
1. `GetWarehouse("50 spaces")` creates a **new, separate warehouse**.
2. `FUN_101d0950` (cmd 0x0F) is called on this **wrong warehouse**.
3. If the wrong warehouse is in server mode (connected to the same WHServer),
   `FUN_101d0950` may succeed (WHServer serves all warehouses).
4. `FUN_101c09f0(0xC)` dispatches cmd 0x4E on the **wrong warehouse**.
5. **Our CLI's warehouse cache is NOT refreshed.**

**Conclusion**: `verifyModule` does **NOT** trigger the cache reset on our CLI's
warehouse. The cache-staleness problem affects **ALL component types** (Strategy,
Indicator, Function, TradingApp), not just Function. The previous partial conclusion
that "verifyModule triggers the cache reset for non-Function types" was **incorrect**
because it did not account for the path mismatch.

### 5.3 Why Does It Work in TSDev.exe?

In TSDev.exe, the warehouse is likely created via `GetWarehouse("50 spaces")` during
TSDev's initialization sequence (before any `GetWarehouse(NULL)` call). This makes the
"50 spaces" warehouse the primary warehouse. When `FUN_101a8620` calls
`GetWarehouse("50 spaces")`, it finds the existing warehouse, and the cache reset
operates on the correct warehouse.

Our CLI creates the warehouse via `GetWarehouse(NULL)` (from `findExistPos` or
`_LoadMasterRecsEX@16`), which registers it under the CSIDL path — a different path
than "50 spaces".

---

## 6. Approach Evaluation (C–G)

### Approach C: Direct `FUN_101c09f0` Call

**Concept**: Resolve `FUN_101c09f0` by signature scan, construct a module object,
and call `FUN_101c09f0(module, 0xC)` directly.

**Feasibility**: MODERATE but complex.

| Requirement | Status |
|-------------|--------|
| Module object with `[module+0xC]` = our warehouse | Must call `FUN_101d0950` (cmd 0x0F) with a known ProgId |
| `[module+0x10]` = COM interface | Set by `FUN_101d0950` via `FUN_101c4ab0` |
| `FUN_101c09f0` resolution | New signature scan needed |
| `FUN_101d0950` INT3 patch | Already patched (offset 67) |
| `FUN_101cfce0` INT3 patch | Only needed if `[module+0x10] == 0` — skipped if `FUN_101d0950` succeeds |
| Sync object at `[module+0x218]` | Must be valid or `CSingleLock` will crash |
| CELProgramWriter initialization | Must call `FUN_1000dd30` to init the module object |

**Safety**: MEDIUM. Requires constructing internal objects with correct layout.
The `CSingleLock` at `[module+0x218]` is a concern — if invalid, the lock call may crash.

**Complexity**: HIGH. Must resolve 3+ internal functions, construct a module object,
and handle the sync object.

**Reliability**: Uncertain across component types. The type code `0xC` is used by
`FUN_101a8620` for all non-Function types, but whether it's correct for Function and
TradingApp is unconfirmed.

### Approach D: Direct `FUN_101cf5b0(warehouse, 0x4E)` with Manual Field Setup

**Concept**: Use our warehouse pointer (from `GetWarehouse(NULL)`), manually set
`[warehouse+0x5E]` and `[warehouse+0x12]`, then call `FUN_101cf5b0(warehouse, 0x4E)`.

**Feasibility**: MODERATE.

| Requirement | Status |
|-------------|--------|
| Warehouse pointer | Already resolved (`m_pGetWarehouse`) |
| `FUN_101cf5b0` INT3 patch | Already patched (offset 36) |
| COM interface for `[warehouse+0x5E]` | Must obtain via cmd 0x0F or cmd 0x0D |
| `FUN_101cfce0` (cmd 0x0D) INT3 patch | **NEW patch needed** (offset 40) |
| `FUN_101cfce0` resolution | New signature scan needed |
| Type code | `0xC` (program-list cache) |

**The COM interface problem**: `[warehouse+0x5E]` must be a valid IWHouse COM interface
pointer. In the normal flow, this is obtained by dispatching cmd 0x0F (which returns the
COM interface in `[warehouse+0x5E]`). We could:

1. Call `FUN_101d0950` (cmd 0x0F) on our warehouse with a known ProgId — this sets
   `[warehouse+0x5E]` to the COM interface result.
2. Then set `[warehouse+0x12] = 0xC` and call `FUN_101cf5b0(warehouse, 0x4E)`.

But `FUN_101d0950` requires constructing a type array and module object, bringing back
the complexity of Approach C.

Alternatively, `FUN_101cfce0` (cmd 0x0D) returns `[warehouse+0x5E]` and clears it. We
could call `FUN_101cfce0`, save the result, then manually set `[warehouse+0x5E]` back
and dispatch cmd 0x4E. But `FUN_101cfce0` has an unpatched INT3.

**Safety**: LOW-MEDIUM. Manual warehouse field manipulation is fragile. If the COM
interface is invalid, the vtable call in `FUN_101cf710` will crash.

**Complexity**: MEDIUM-HIGH. Requires 1 new patch, 1 new signature, manual field
manipulation, and SEH-protected calls to internal functions.

**Reliability**: Same type-code concern as Approach C.

### Approach E: Close and Reinitialize Warehouse

**Concept**: Call `closeWarehouse()` (or `OrlibExit`) followed by `OrlibInit()` to
re-populate caches from WHServer.

**Feasibility**: LOW.

| Issue | Detail |
|-------|--------|
| `OrlibInit` is reference-counted | `DAT_1023acb0` is incremented on each call; second call returns `true` without reinitializing |
| Must call `OrlibExit` first | `OrlibExit` may destroy the RTE and other global state |
| `closeWarehouse` bug | Known uninitialized-buffer issue in `_CloseWarehouseForCurrentThreadAndPath@4` |
| Re-initialization safety | Uncertain whether re-init within one session is safe |

**Safety**: LOW. `OrlibExit` + `OrlibInit` may corrupt global state. The
`closeWarehouse` bug adds risk.

**Complexity**: LOW (conceptually), but HIGH risk.

**Reliability**: Uncertain. May work but could also destabilize the session.

### Approach F: Invoke `FUN_101a8620` with NULL Path

**Concept**: Resolve `FUN_101a8620` by signature scan and call it with `param_3 = 0`
(NULL) instead of the 50-space string. This would cause `GetWarehouse(NULL)` to find
our warehouse, making the cache reset operate on the correct warehouse.

**Feasibility**: MODERATE.

| Requirement | Status |
|-------------|--------|
| `FUN_101a8620` resolution | New signature scan needed |
| `FUN_101d0950` INT3 patch | Already patched (offset 67) |
| `FUN_101cf5b0` INT3 patch | Already patched (offset 36) |
| `FUN_101cfce0` INT3 patch | Not needed (skipped if `FUN_101d0950` succeeds) |
| Known ProgId | Available from `createEntry` or `findExistPos` |
| `DAT_1023aaac` global state | Must be != 1 for the function to proceed |

**Side effects**: `FUN_101a8620` processes module elements in a loop (calling
`FUN_101a4f90` for each element). If called after `verifyModule` (which has already
compiled and verified the program), this processing should be a no-op (the program is
already in its final state). Additional cleanup calls (`FUN_10006a00`,
`FUN_101c0fd0`, `FUN_101c00c0`) are CELProgramWriter lifecycle methods.

**Safety**: MEDIUM. The function is designed to be called with any valid warehouse and
ProgId. Using NULL path is a legitimate input to `GetWarehouse`. The main risk is the
module element processing loop having unintended side effects.

**Complexity**: MEDIUM. Requires 1 new signature, SEH-protected `__fastcall` call.

**Reliability**: Good for non-Function types (Strategy, Indicator, TradingApp).
Function (category 4) is skipped by `_VerifyModule_New@20` but `FUN_101a8620` itself
does NOT check for category 4 — it's the caller that skips. So calling `FUN_101a8620`
directly with a Function ProgId should work, but this is unconfirmed.

### Approach G: Live-Lookup Enumeration via `_GetMasterRecByType@16`

**Concept**: Use the exported `_GetMasterRecByType@16` (ordinal 510, RVA `0x1A0460`)
for live record retrieval instead of the cached `_LoadMasterRecsEX@16`.

**Feasibility**: HIGH — this is the recommended approach from the existing findings
(`RE_F5_cache_fix_findings.md`), with corrections from the adversarial review.

#### 6.1 How `_GetMasterRecByType@16` Works

```c
undefined4 _GetMasterRecByType_16(
    int category,      // [EBP+0x08]
    int subtype,       // [EBP+0x0C] — power-type input to ConvertToPowerType
    int index,         // [EBP+0x10] — ProgId (NOT sequential index)
    undefined4 *outBuf // [EBP+0x14] — caller-allocated 0x10A-byte buffer
)
{
    iVar1 = FUN_101a9650(0);              // GetWarehouse(NULL) — uses OUR warehouse
    uVar2 = FUN_101d4280(category, subtype); // expand to warehouse type
    puVar3 = FUN_101cfe00(uVar2, index, 0);  // live lookup
    // ... copy 0x10A bytes to outBuf ...
    return 1;  // success
}
```

#### 6.2 Server-Mode BST Fallback

`FUN_101cfe00` checks `[warehouse+0xC2]` for server mode:

- **Server mode** (`[warehouse+0xC2] == 1`): Calls `FUN_101e3990(type, index)`, which
  does a BST lookup by ProgId. If the item is NOT in the BST (new items not in cache),
  it **falls back to `FUN_101d15c0`** (cmd 0x0D, live lookup).

- **Local mode**: Directly calls `FUN_101d15c0` (cmd 0x0D).

The fallback in `FUN_101e3990` (at `0x101E3A2E`):

```asm
101e3a22  PUSH 0                    ; NULL path
101e3a24  CALL 0x101a9650           ; GetWarehouse(NULL) → our warehouse
101e3a2c  MOV  ECX, EAX             ; this = warehouse
101e3a2e  CALL 0x101d15c0           ; FUN_101d15c0 — cmd 0x0D dispatch
```

This fallback uses `GetWarehouse(NULL)` — our warehouse. The `FUN_101d15c0` INT3
(offset 84) **must be patched** for this to work from our CLI thread.

#### 6.3 Index Parameter is ProgId, Not Sequential Position

The adversarial review (`RE_F5_cache_fix_review.md` Finding 2) established that the
`index` parameter is the **ProgId**, not a sequential position. Evidence:

1. The only code caller (`CompileText` at `0x1001E19B`) passes the ProgId as `param_3`.
2. `FUN_101e3990` (server mode) uses a BST keyed by ProgId — `puVar3[4]` (offset 0x10)
   is compared with `param_3` (the index).
3. The cmd 0x0D worker thread handler calls `IWHouse::ProgramById` (vtable 0x20),
   which is a ProgId-based lookup.

This means iterating from 0 iterates through ProgId space, not sequential positions.
Deleted items leave ProgId gaps. A fixed gap threshold (e.g., 10) is insufficient for
large gaps.

#### 6.4 Count Mechanism Options

| Option | Description | Feasibility |
|--------|-------------|-------------|
| **Use `_LoadMasterRecsEX@16` count as upper bound** | The cached count gives the number of items at init time. New items have ProgIds >= this count (ProgIds are monotonically assigned). Iterate from 0 to `cached_count + new_items_count`. | Simple, but requires tracking new items |
| **Use `FUN_101d0300` (cmd 0x19) for next ProgId** | Returns the next available ProgId — an upper bound on ProgId space. | Needs new patch (INT3 at thread check) + signature |
| **Large gap threshold** | Iterate with a large threshold (e.g., 1000+). | Not guaranteed correct |
| **Hybrid: cached enumeration + `findExistPos` verification** | Enumerate cached records for name candidates, verify each with `findExistPos` (live, exact-match). | No new patches, but only works for exact-match verification, not pure enumeration |

#### 6.5 Multi-Subtype Enumeration

The adversarial review (`RE_F5_cache_fix_review.md` Finding 3) noted that
`_LoadMasterRecsEX@16` enumerates ALL type codes for a category (via `FUN_101d43a0`),
while `_GetMasterRecByType@16` with a single subtype only looks up ONE type code.

For user-created items (which always have subtype 0), this is not an issue — they all
have the base type (0 for Strategy, 2 for Indicator, 4 for Function). But built-in
items with other subtypes would be missed.

For the `exists` use case, this is acceptable: user-created items always have subtype 0,
and the primary use case for case-insensitive `exists` is finding user-created items.

#### 6.6 Patches Required

| Patch | Function | Command | Offset | Status |
|-------|----------|---------|--------|--------|
| `FUN_101d15c0_thread_check_INT3` | `FUN_101d15c0` | 0x0D | 84 | **NEW** — CC→90 |
| `FUN_101cf5b0_thread_check_INT3` | `FUN_101cf5b0` | 0x35 (lazy-init) | 36 | Already patched |

#### 6.7 Assessment

| Criterion | Rating |
|-----------|--------|
| Feasibility | HIGH — exported function, handles warehouse access internally |
| Safety | HIGH — no manual object construction, no COM interface manipulation |
| Complexity | LOW-MEDIUM — 1 new patch, 1 new export, iteration logic |
| Reliability | MEDIUM — ProgId-based iteration requires careful gap handling |

---

## 7. Recommendation

### Primary Recommendation: Approach G (Live-Lookup Enumeration via `_GetMasterRecByType@16`)

Approach G remains the recommended approach, consistent with the existing findings in
`RE_F5_cache_fix_findings.md`, with the following corrections from the adversarial
review and this analysis:

1. **Use `getSubType(type)` (not a separate mapping)** — The correct subtypes are
   Strategy=0, Indicator=0, Function=0, TradingApp=0x14. The previously proposed
   values (Strategy=2, Indicator=5) were incorrect.

2. **Handle ProgId-based iteration** — The index parameter is the ProgId, not a
   sequential position. Use the cached `_LoadMasterRecsEX@16` count as a starting
   upper bound, and extend it by the number of items created in the current session
   (tracked via `createEntry` calls). Use a generous gap threshold (e.g., 100) as a
   safety net.

3. **Patch `FUN_101d15c0` (cmd 0x0D, offset 84)** — Required for the BST fallback
   path in server mode, which is triggered for items not in the cached BST.

4. **Document the subtype-0 limitation** — Only subtype-0 items are enumerated. This
   is sufficient for user-created items (which always have subtype 0) but may miss
   built-in items with other subtypes.

### Secondary Option: Approach F (Invoke `FUN_101a8620` with NULL Path)

If the cache-refresh approach is preferred (making the existing `_LoadMasterRecsEX@16`
enumeration see new items), Approach F is the most promising:

- Call `FUN_101a8620(category, progId, 0)` with `param_3 = 0` (NULL path)
- This makes `GetWarehouse(NULL)` find our warehouse, so the cache reset operates on
  the correct warehouse
- Requires 1 new signature (for `FUN_101a8620`), no new patches
- Must be called after `verifyModule` with a known ProgId
- Side effects (module element processing) should be benign post-verification

However, Approach F is more complex and riskier than Approach G, and its reliability
for Function and TradingApp types is unconfirmed.

### Approaches Not Recommended

- **Approach C**: Too complex (module object construction, sync object setup)
- **Approach D**: COM interface manipulation is fragile
- **Approach E**: `OrlibExit`/`OrlibInit` cycle risks destabilizing the session

---

## 8. Implementation Guidance (Approach G)

### 8.1 Add Export Entry

Add to `signatures/v9.50.01.3344.json` → `exports` array:

```json
{
  "name": "_GetMasterRecByType@16",
  "rva": "0x1A0460",
  "callingConvention": "stdcall"
}
```

### 8.2 Add INT3 Patch Signature

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
    "author": "RE-F5-frida-trace",
    "date": "2026-08-27",
    "source": "Ghidra disassembly",
    "reference": "docs/RE/RE_F5_frida_trace_analysis.md"
  }
}
```

### 8.3 Type Definition

```cpp
// _GetMasterRecByType@16 — __stdcall, 4 params (category, subtype, index, outBuf)
// Returns 1 on success (record found), 0 on failure (not found / warehouse unavailable)
// outBuf must be 0x10A (266) bytes, caller-allocated
// index is the ProgId (NOT a sequential position)
typedef int (__stdcall *GetMasterRecByTypeFn)(int category, int subType,
                                              int index, void* outBuf);
```

### 8.4 Subtype Mapping

**Use the existing `getSubType(type)` function** — do NOT create a separate mapping.

```cpp
// Correct subtypes for _GetMasterRecByType@16 (same as getSubType):
//   Strategy:   0    → expanded type 0
//   Indicator:  0    → expanded type 2
//   Function:   0    → expanded type 4
//   TradingApp: 0x14 → expanded type 0x1402
```

### 8.5 Iteration Strategy

```cpp
// 1. Get cached count from _LoadMasterRecsEX@16 (for upper bound estimation)
// 2. Track ProgIds assigned during this session (from createEntry return values)
// 3. Iterate from 0 to max(cachedCount + sessionCreatedCount + safetyMargin, 100)
// 4. For each index, call _GetMasterRecByType@16(category, subType, index, buf)
// 5. If return == 1: process record (name at 0x17, ProgId at 0x00)
// 6. If return == 0: increment consecutive miss counter
// 7. Stop when consecutive misses exceed threshold (e.g., 100)
```

### 8.6 Patch Restoration

The `m_threadCheckRvas` array (currently size 6) must be increased to 7, and the
destructor's restoration loop must be updated accordingly.

### 8.7 SEH-Protected Call Wrapper

```cpp
struct GetMasterRecByTypeSehResult {
    int result = 0;
};

bool callGetMasterRecByType(GetMasterRecByTypeFn fn, int cat, int sub,
                            int idx, void* buf, GetMasterRecByTypeSehResult* out) {
    out->result = 0;
    __try {
        out->result = fn(cat, sub, idx, buf);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
```

---

## 9. Evidence Index

| Function | RVA | Key Finding |
|----------|-----|-------------|
| `FUN_101a8620` | `0x1A8620` | Save/compile orchestrator containing the cache-reset call; uses `GetWarehouse(param_3)` with 50-space string |
| `FUN_101c09f0` | `0x1C09F0` | Cache reset: invalidates cache flags, sets `[warehouse+0x5E]` and `[warehouse+0x12]`, dispatches cmd 0x4E |
| `FUN_101cf710` | `0x1CF710` | Worker thread cmd 0x4E handler: calls IWHouse vtable 0x48 with type code |
| `FUN_101cf5b0` | `0x1CF5B0` | Generic command dispatcher (already INT3-patched at offset 36) |
| `FUN_101cfce0` | `0x1CFCE0` | Cmd 0x0D dispatcher; unpatched INT3 at offset 40; skipped when `[module+0x10] != 0` |
| `FUN_101d0950` | `0x1D0950` | Cmd 0x0F (load program); INT3-patched at offset 67; sets `[module+0x10]` via `FUN_101c4ab0` |
| `FUN_101c4ab0` | `0x1C4AB0` | Sets `[module+0x0C]` = warehouse, `[module+0x10]` = COM interface |
| `FUN_101a9650` | `0x1A9650` | GetWarehouse: NULL → CSIDL path; non-NULL → exact path match; creates new warehouse if no match |
| `_VerifyModule_New@20` | `0x169630` | Calls `FUN_101a8620` with 50-space string for non-Function types; skips for Function |
| `FUN_101a8360` | `0x1A8360` | Delete orchestrator; calls `FUN_101c09f0(0xB)` and `FUN_101c09f0(0xC)` for category 0/1 |
| `FUN_1016f380` | `0x16F380` | Full save flow; calls `FUN_101c09f0` with type codes 0xA, 0x9, 0xC, 0x11 |
| `_GetMasterRecByType@16` | `0x1A0460` | Export ordinal 510; live lookup via cmd 0x0D; uses `GetWarehouse(NULL)` |
| `FUN_101cfe00` | `0x1CFE00` | Record retrieval: server mode → BST lookup with `FUN_101d15c0` fallback; local mode → `FUN_101d15c0` |
| `FUN_101e3990` | `0x1E3990` | Server-mode BST lookup by ProgId; falls back to `FUN_101d15c0` (cmd 0x0D) for items not in BST |
| `FUN_101d15c0` | `0x1D15C0` | Cmd 0x0D local-mode dispatcher; INT3 at offset 84; **needs new patch** |
| `FUN_101d0300` | `0x1D0300` | Cmd 0x19 (get next ProgId); has unpatched INT3; called by `_CreateEntryEX@24` |
| `s__10230710` | `0x230710` | 50-byte string of spaces (0x20) — hardcoded path used by all callers of `FUN_101a8620` |
| `FUN_101d04f0` | `0x1D04F0` | Record retrieval helper: server mode → `FUN_101e3990`; local mode → `FUN_101d15c0` |
| `FUN_101d0590` | `0x1D0590` | Program object retrieval: server mode → `FUN_101e3c50`; local mode → `FUN_101d17b0` |
| `FUN_101d17b0` | `0x1D17B0` | Cmd 0x0F local-mode dispatcher; unpatched INT3 at offset 0x62 (not needed in server mode) |

---

> **This document is research-only.** No production source, signature files, or
> tests were modified during this investigation.
