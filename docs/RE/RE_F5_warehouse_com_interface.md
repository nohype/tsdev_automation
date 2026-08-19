# RE F5: Warehouse COM Interface (`[warehouse+0x5E]`) — Why It Stays NULL

## 1. Executive Summary

The prior document (`docs/RE/archive/RE_F5_cache_refresh_feasibility.md`) claimed that cmd 0x0E
(used by `saveSource`) writes `[warehouse+0x5E]` (the IWHouse COM interface
pointer). **This is incorrect.** Direct Ghidra decompilation of the worker
handler `FUN_101d2150` proves that cmd 0x0E **does not write**
`[warehouse+0x5E]` at all — it only stores a status code at `[warehouse+0x4E]`.

Only four commands write `[warehouse+0x5E]`: **0x09, 0x0D, 0x0F, and 0x10**,
and only conditionally (on successful COM server calls). In our warehouse,
which operates in **server mode** (`[warehouse+0xC2] = 1`), the indirect
lookup paths used by `findExistPos`, `saveSource`, and `verifyModule` take the
BST cache branch and never dispatch these commands. The only operations that
dispatch pointer-setting commands are `createEntry` (cmd 0x09 via
`FUN_101cf990`/`FUN_101d0860`) and `deleteEntry` (cmd 0x0F via
`FUN_101d0950`).

Runtime diagnostics confirming `[warehouse+0x5E]` remains NULL after
`findExistPos`, `createEntry`, `saveSource`, and `verifyModule` are consistent
with the server-mode finding: the BST cache paths bypass the command
dispatches that would set the pointer.

**Recommended approach**: dispatch cmd 0x0F directly via `FUN_101d0950`
(already INT3-patched) using a known ProgId and type array, immediately before
the cmd 0x4E cache-refresh dispatch. This requires resolving two additional
internal function addresses (`FUN_101d0950` entry point and `FUN_101d43a0`
type-array builder) via new signatures.

---

## 2. Warehouse Field Reference

| Offset | Ghidra notation | Purpose |
|--------|----------------|---------|
| 0x04 | `param_1[0]._0_4_` | Command ID (set by dispatcher before signaling worker) |
| 0x08 | `param_1[0]._4_4_` | Name / identifier parameter |
| 0x12 | `param_1[1]._4_4_` | Type code / ProgId parameter |
| 0x16 | `param_1[1]._8_4_` | Secondary type parameter |
| 0x1E | `param_1[2]._2_2` | Flags (set to 0x2000 by dispatchers) |
| 0x26 | `param_1[2]._8_4_` | SAFEARRAY pointer |
| 0x4E | `param_1[6]._2_4` | COM call status (0 = success) |
| 0x52 | `param_1[6]._6_4_` | Init status (cmd 0x01) |
| 0x5E | `param_1[7]._2_4` | **IWHouse COM interface pointer** (the field under investigation) |
| 0x6A | `param_1[8]._2_4` | Worker completion event |
| 0x6E | `param_1[8]._6_4_` | Worker signal event |
| 0x72 | `param_1[9]._2_4` | Warehouse mutex |
| 0x76 | `param_1[9]._6_4_` | Owner thread ID |
| 0xAE | `param_1[0xe]._2_4` | COM server pointer (WHServer) |
| 0xB2 | `param_1[0xe]._6_4_` | Init-done flag |
| 0xC2 | `param_1[0xc]._2_4` | Mode flag: 0 = local, 1 = server |

Ghidra types `param_1` as `IUnknownVtbl *` (12 bytes per element), so byte
offset N appears as `param_1[N/12].<field> + (N%12)`.

---

## 3. Warehouse Initialization

### 3.1 Constructor (`FUN_101cf1f0`, RVA 0x1CF1F0)

```c
*(undefined4 *)((int)param_1 + 0xae) = 0;   // COM server = NULL
*(undefined4 *)((int)param_1 + 0xc2) = 0;   // mode = LOCAL
*(undefined4 *)((int)param_1 + 0x5e) = 0;   // COM interface = NULL
*(DWORD *)((int)param_1 + 0x76) = GetCurrentThreadId();  // owner thread
```

All three critical fields start at 0 (NULL). The owner thread is set to the
creating thread.

### 3.2 Initializer (`FUN_101a9330`, called by `FUN_101a9650`/GetWarehouse)

When `GetWarehouse(NULL)` is called (path = NULL):

```c
if (param_2 == (char *)0x0) {        // path is NULL
    if (DAT_1023aa98 == 0) {
        *(undefined4 *)(param_1 + 0xc2) = 1;  // SERVER MODE
    }
    // ... get default folder path ...
}
// ...
iVar3 = FUN_101d1310(iVar3);
if (iVar3 != 0) {
    uVar5 = FUN_101d1c00(local_14, param_3);  // dispatch cmd 0x01
}
```

**Key finding**: `GetWarehouse(NULL)` sets `[warehouse+0xC2] = 1` (server
mode). This is our warehouse — we call `GetWarehouse(NULL)` via
`callGetWarehouse(m_pGetWarehouse, 0, &warehouse)`.

### 3.3 Cmd 0x01 (warehouse init, worker handler `FUN_101d2150`)

Cmd 0x01 initializes the COM server:

```c
// cmd 0x01 handler (extracted from FUN_101d2150):
FUN_101d1240();
FUN_101cf050();
// ...
iVar8 = local_1c;
*(int *)((int)&param_1[0xe].AddRef + 2) = iVar8;  // [warehouse+0xAE] = COM server
if (iVar8 == 0) {
    // init failed
    *(undefined4 *)((int)&param_1[6].Release + 2) = 0;  // [warehouse+0x4E] = 0
} else {
    // init succeeded — set up connection advisor
    // ...
}
```

After cmd 0x01 succeeds, `[warehouse+0xAE]` is set to the WHServer COM
interface pointer. All subsequent command dispatchers check
`[warehouse+0xAE] != 0` before dispatching.

**After initialization:**
- `[warehouse+0xAE]` = valid COM server pointer (set by cmd 0x01)
- `[warehouse+0xC2]` = 1 (server mode)
- `[warehouse+0x5E]` = 0 (still NULL — cmd 0x01 does not set it)

---

## 4. Worker Handler Analysis — Which Commands Write `[warehouse+0x5E]`

The worker handler `FUN_101d2150` (RVA 0x1D2150) contains a large switch
statement. A comprehensive search of the decompiled code for all writes to
`param_1[7].Release + 2` (i.e., `[warehouse+0x5E]`) found exactly four
command cases that write it.

### 4.1 Commands That WRITE `[warehouse+0x5E]`

All four follow the same pattern:

```c
piVar9 = *(int **)((int)&param_1[0xe].AddRef + 2);  // [warehouse+0xAE] = COM server
iVar8 = (**(code **)(*piVar9 + VTABLE_OFFSET))(piVar9, ...);  // COM server call
*(int *)((int)&param_1[6].AddRef + 2) = iVar8;  // [warehouse+0x4E] = status
if (iVar8 != 0) goto LAB_101d285d;  // failure → clear [warehouse+0x5E]
// success path:
FUN_101cf020();  // COM QueryInterface — get IWHouse interface
*(int *)((int)&param_1[7].Release + 2) = local_XX;  // [warehouse+0x5E] = interface
// Release temporary COM object
// ...

LAB_101d285d:  // shared failure label
    *(undefined4 *)((int)&param_1[7].Release + 2) = 0;  // [warehouse+0x5E] = NULL
```

| Command | Vtable Offset | Local Var | Classification |
|---------|-------------|------------|----------------|
| 0x09 | 0x24 | local_24 | CONDITIONAL: success → set, failure → clear |
| 0x0D | 0x20 | local_2c | CONDITIONAL: success → set, failure → clear |
| 0x0F | 0xA8 | local_34 | CONDITIONAL: success → set, failure → clear |
| 0x10 | 0xAC | local_3c | CONDITIONAL: success → set, failure → clear |

`FUN_101cf020` is a COM QueryInterface wrapper: it calls `FUN_101cf090` which
invokes `QueryInterface` on a COM object to obtain the IWHouse interface
pointer. If the QI fails, `_com_issue_error` is called (which may throw,
caught by SEH).

### 4.2 Commands That READ but Do NOT Write `[warehouse+0x5E]`

These commands dereference `[warehouse+0x5E]` to call vtable methods on the
IWHouse interface. They require the pointer to already be valid.

| Command | Vtable Offset | Action |
|---------|-------------|--------|
| 0x32 | 0x1C | Call IWHouse method |
| 0x33 | 0x2C | Call IWHouse method |
| 0x34 | 0x20 | Call IWHouse method |
| 0x35 | 0x24 | Call IWHouse method |
| 0x36 | 0x58 | Call IWHouse method |
| 0x37 | 0x30 | Call IWHouse method |
| 0x39 | 0x54 | Call IWHouse method |
| 0x3A | 0x40 | Call IWHouse method |
| 0x3B | 0x38 | Call IWHouse method |
| 0x41 | 0x70 | Call IWHouse method |
| 0x42 | 0x74 | Call IWHouse method (with params) |
| 0x43 | 0x78 | Call IWHouse method |
| 0x44 | 0x7C | Call IWHouse method (with params) |
| 0x45 | 0x80 | Call IWHouse method |
| 0x46 | 0x84 | Call IWHouse method (with params) |
| 0x47 | 0x28 | Call IWHouse method (with params) |
| 0x48 | 0x5C | Call IWHouse method (with params) |
| 0x49 | 0x34 | Call IWHouse method (with params) |
| 0x4B | 0x44 | Call IWHouse method (with params) |
| 0x4C | 0x3C | Call IWHouse method (with params) |
| 0x4F | 0x08 | Release IWHouse |
| 0x50 | 0x04 | AddRef IWHouse |
| 0x51 | 0x90 | Call IWHouse method |
| 0x52 | 0x94 | Call IWHouse method (with params) |
| 0x53 | 0x98 | Call IWHouse method |
| 0x54 | 0x9C | Call IWHouse method (with params) |
| 0x55 | 0xA0 | Call IWHouse method |
| 0x56 | 0xA4 | Call IWHouse method (with params) |
| 0x57 | 0xA8 | Call IWHouse method |
| 0x58 | 0xAC | Call IWHouse method (with params) |
| 0x65 | 0x88 | Call IWHouse method |
| 0x66 | 0x8C | Call IWHouse method (with params) |
| 0x77 | 0xB4 | Call IWHouse method (with params) |
| 0x78 | 0xB0 | Call IWHouse method (with params) |
| 0x79 | 0xBC | Call IWHouse method |
| 0x7A | 0xB8 | Call IWHouse method |

**All of these would crash if `[warehouse+0x5E]` is NULL.** Cmd 0x4E (cache
refresh) is among these — it calls `FUN_101cf710` which dereferences the
IWHouse interface.

### 4.3 Command 0x0E — Does NOT Write `[warehouse+0x5E]`

```c
case 0x0E:
    piVar9 = *(int **)((int)&param_1[0xe].AddRef + 2);  // [warehouse+0xAE]
    uVar11 = (**(code **)(*piVar9 + 0x3c))               // vtable +0x3C
        (piVar9, ...);
    *(undefined4 *)((int)&param_1[6].AddRef + 2) = uVar11; // [warehouse+0x4E] = status
    break;  // NO write to [warehouse+0x5E]
```

Cmd 0x0E calls the COM **server** (vtable +0x3C), not the IWHouse interface.
It stores only the status at `[warehouse+0x4E]`. It does not call
`FUN_101cf020` and does not write `[warehouse+0x5E]`.

**This directly contradicts the prior document's table (§4.1) which listed
cmd 0x0E as writing `[warehouse+0x5E]`.**

### 4.4 Command 0x17 — Does NOT Write `[warehouse+0x5E]`

```c
case 0x17:
    uVar11 = FUN_101d2000(...);
    *(undefined4 *)((int)&param_1[6].AddRef + 2) = uVar11;  // [warehouse+0x4E]
    break;  // NO write to [warehouse+0x5E]
```

### 4.5 Command 0x4E — Reads `[warehouse+0x5E]` (would crash if NULL)

```c
case 0x4E:
    FUN_101cf710();
    break;
```

`FUN_101cf710` dereferences the IWHouse interface from `[warehouse+0x5E]` to
call vtable method 0x48 (cache refresh). If `[warehouse+0x5E]` is NULL, this
crashes with an access violation.

### 4.6 Command 0x4F — Releases `[warehouse+0x5E]`

```c
case 0x4F:
    piVar9 = *(int **)((int)&param_1[7].Release + 2);  // read [warehouse+0x5E]
    if (piVar9 != (int *)0x0) {
        (**(code **)(*piVar9 + 8))();  // Release()
    }
    break;
```

Cmd 0x4F calls `Release()` on the IWHouse interface but does not set the
field to NULL afterward. It is a read-only operation on the field.

---

## 5. CLI Operation Flow Analysis

### 5.1 `callGetWarehouse` (src/tsdev_lib.cpp:233)

```cpp
bool callGetWarehouse(GetWarehouseFn fn, int pathParam, void** out) {
    *out = fn(pathParam);  // GetWarehouse(0) = GetWarehouse(NULL)
}
```

`GetWarehouse(NULL)` returns our warehouse (server mode, `[warehouse+0xC2]=1`).
It does not dispatch any commands. The COM server `[warehouse+0xAE]` was
already initialized during the first `GetWarehouse(NULL)` call (which
dispatched cmd 0x01 internally).

### 5.2 `findExistPos` (src/tsdev_lib.cpp:677)

Calls the `FindExistPos` export → `FUN_101d05e0`:

```c
if (*(int *)(param_1 + 0xc2) == 1) {   // SERVER MODE
    FUN_101a9460(param_2, param_3);
    iVar1 = FUN_101e3db0(param_2, param_3);  // BST cache lookup
    *param_4 = iVar1;
    return *param_4 != 0;
}
FUN_101d18c0(param_2, param_3, param_4);  // local mode: cmd 0x10
```

**In server mode**: BST cache lookup. Cmd 0x10 is NOT dispatched.
`[warehouse+0x5E]` is NOT set.

### 5.3 `createEntry` (src/tsdev_lib.cpp:687)

Calls `_CreateEntryEX@24` → `FUN_101cf990` (cmd 0x16 + cmd 0x09):

`FUN_101cf990` is NOT behind a server-mode check. It is called directly by
`_CreateEntryEX@24` with the warehouse as `this` (ECX). It dispatches:
1. Cmd 0x16 (does NOT write `[warehouse+0x5E]`)
2. If cmd 0x16 succeeds, calls `FUN_101d0860` which:
   - Checks `[warehouse+0xAE] != 0` (COM server must be initialized)
   - Dispatches cmd 0x09 (SETS `[warehouse+0x5E]` on success)
   - If `[warehouse+0x5E] != 0`, allocates a module object and returns 1

`FUN_101cf990` and `FUN_101d0860` both have thread checks (INT3) that are
NOT in the patch list. However, `[warehouse+0x76]` is set to the creating
thread's ID (our CLI thread), so the thread check passes.

**After createEntry: `[warehouse+0x5E]` MAY be set** (if cmd 0x09's COM
server call succeeds). This is the one path in the insert/modify flow that
can set the pointer.

Then `_CreateEntryEX@24` calls `FUN_101d3c00` (cmd 0x0E), which does NOT
write `[warehouse+0x5E]`.

### 5.4 `saveSource` (src/tsdev_lib.cpp:710)

Calls `_SaveModule_New@20` → `FUN_10159770` → `FUN_101cff60`:

```c
// FUN_101cff60
if (*(int *)(param_1 + 0xc2) == 1) {   // SERVER MODE
    FUN_101a9460(param_2, param_3);
    puVar3 = FUN_101e3c50(param_2, param_3);  // BST cache lookup
}
else {
    FUN_101d17b0(param_2, param_3, &local_8);  // local mode: cmd 0x0F
}
```

**In server mode**: BST cache lookup. Cmd 0x0F is NOT dispatched.

Then `FUN_101a07f0` dispatches cmd 0x0E (via `FUN_101d3c00`), which does NOT
write `[warehouse+0x5E]`. If cmd 0x0E fails, the fallback calls
`FUN_101cf990` (cmd 0x16 + cmd 0x09), which CAN set `[warehouse+0x5E]`. But
if cmd 0x0E succeeds (the normal case), the fallback is NOT triggered.

**After saveSource: `[warehouse+0x5E]` is NOT set** (cmd 0x0E does not
write it, and the BST cache path does not dispatch cmd 0x0F).

### 5.5 `verifyModule` (src/tsdev_lib.cpp:834)

Calls `_VerifyModule_New@20` which:
1. Calls `FUN_101cff60` — **server mode**: BST cache, no cmd 0x0F
2. Calls `FUN_101a8620` with a path string — this calls `GetWarehouse(path)`
   which returns a **DIFFERENT warehouse** (not our NULL-path warehouse).
   Cmd 0x0F is dispatched on the different warehouse, not ours.
3. Calls `FUN_101a07f0` — dispatches cmd 0x0E on our warehouse (does NOT
   write `[warehouse+0x5E]`)

**After verifyModule: `[warehouse+0x5E]` is NOT set** on our warehouse.

### 5.6 `deleteEntry` (via `FUN_101a8360`)

Calls `FUN_101d0950` directly (NOT behind a server-mode check):
- `FUN_101d0950` is already INT3-patched
- Dispatches cmd 0x0F (SETS `[warehouse+0x5E]` on success)
- Then dispatches cmd 0x17 (SAVE/COMMIT, does NOT write `[warehouse+0x5E]`)

**After deleteEntry: `[warehouse+0x5E]` IS set** (by cmd 0x0F).

### 5.7 Summary Table

| Operation | Commands Dispatched | Sets `[warehouse+0x5E]`? |
|-----------|---------------------|--------------------------|
| findExistPos | None (BST cache, server mode) | NO |
| createEntry | 0x16, 0x09, 0x0E | **CONDITIONAL** (0x09 on success) |
| saveSource | 0x0E (via FUN_101cff60 BST + FUN_101a07f0) | NO (0x0E doesn't write it) |
| verifyModule | 0x0E (our warehouse), 0x0F (different warehouse) | NO |
| deleteEntry | 0x0F, 0x17 | **YES** (0x0F on success) |
| getSource | 0x11 | NO |
| enumerateRecords | 0x08, 0x07 | NO |

---

## 6. Why `[warehouse+0x5E]` Remains NULL

### 6.1 The Server Mode Bypass

Our warehouse operates in **server mode** (`[warehouse+0xC2] = 1`), set by
`FUN_101a9330` when `GetWarehouse(NULL)` is called. In server mode, the
indirect lookup functions (`FUN_101d05e0`, `FUN_101cff60`, `FUN_101cfe00`)
take the BST cache branch and never dispatch the commands that set
`[warehouse+0x5E]`:

| Function | Server Mode Path | Local Mode Path (cmd) |
|----------|-----------------|----------------------|
| `FUN_101d05e0` (findExistPos) | BST cache (`FUN_101e3db0`) | cmd 0x10 |
| `FUN_101cff60` (save/verify lookup) | BST cache (`FUN_101e3c50`) | cmd 0x0F |
| `FUN_101cfe00` (GetMasterRecByType) | BST cache (`FUN_101e3990`) | cmd 0x0D |

### 6.2 The cmd 0x0E Misconception

The prior document claimed cmd 0x0E writes `[warehouse+0x5E]`. This was the
basis for claiming `saveSource` initializes the pointer. Direct Ghidra
evidence shows cmd 0x0E only calls the COM server (vtable +0x3C) and stores
the status at `[warehouse+0x4E]`. It does NOT call `FUN_101cf020` and does
NOT write `[warehouse+0x5E]`.

### 6.3 The createEntry Exception

`createEntry` dispatches cmd 0x09 via `FUN_101cf990`/`FUN_101d0860`, which
is NOT behind a server-mode check. If the COM server call in cmd 0x09
succeeds, `[warehouse+0x5E]` is set. However, runtime diagnostics show it
remains NULL after createEntry. Possible explanations:

1. **The COM server call fails**: The vtable +0x24 call returns non-zero,
   triggering the failure path which clears `[warehouse+0x5E]` to 0. But
   `FUN_101d0860` would then return 0, `FUN_101cf990` would return 0, and
   `_CreateEntryEX@24` would fail. Since createEntry succeeds, this seems
   unlikely — unless the failure is in `FUN_101cf020` (QI), which calls
   `_com_issue_error` (throwing, caught by SEH in our wrapper).

2. **`FUN_101cf020` throws**: The QueryInterface for IWHouse fails, raising
   a COM error. The SEH handler in our `createEntry` wrapper catches it,
   but `[warehouse+0x5E]` remains 0. The module object allocation in
   `FUN_101d0860` might still succeed via a different path, allowing
   createEntry to return a valid ProgId.

3. **A subsequent operation clears it**: Something between createEntry and
   the diagnostic check clears `[warehouse+0x5E]`. However, no identified
   operation in the insert/modify flow writes 0 to `[warehouse+0x5E]`
   except the failure paths of cmds 0x09/0x0D/0x0F/0x10.

**This requires further runtime investigation** (e.g., Frida tracing of
`[warehouse+0x5E]` during createEntry) to determine the exact cause. The
static analysis confirms that the pointer-setting path exists but is
unreliable.

### 6.4 The `FUN_101cfce0` (cmd 0x0D) Borrow Pattern

`FUN_101cfce0` dispatches cmd 0x0D, which sets `[warehouse+0x5E]` in the
worker. But `FUN_101cfce0` then **reads and clears** the field:

```c
uVar3 = *(undefined4 *)(param_1 + 0x5e);  // read [warehouse+0x5E]
*(undefined4 *)(param_1 + 0x5e) = 0;       // CLEAR [warehouse+0x5E]
return uVar3;                               // return the pointer to caller
```

This is a "borrow" pattern: the caller gets a temporary COM interface
pointer, uses it, and is responsible for releasing it. The warehouse's
`[warehouse+0x5E]` is left at 0 after the call. **`FUN_101cfce0` cannot be
used to initialize `[warehouse+0x5E]` for cmd 0x4E.**

In contrast, `FUN_101d15c0` (also cmd 0x0D, used by `_GetMasterRecByType@16`)
does NOT clear `[warehouse+0x5E]` after reading it. But it is not INT3-patched.

---

## 7. Candidate Initialization Approaches

### Approach A: Dispatch cmd 0x0F via `FUN_101d0950` (RECOMMENDED)

`FUN_101d0950` is already INT3-patched (`FUN_101d0950_thread_check_INT3`). It
is the same function used by the delete orchestrator.

**What it does**:
1. Checks `[warehouse+0xAE] != 0` (COM server initialized — YES after init)
2. Builds a SAFEARRAY from the type array (calls `FUN_101d1010` internally)
3. Sets `[warehouse+0x12]` = ProgId/name
4. Dispatches cmd 0x0F (worker sets `[warehouse+0x5E]` on success)
5. If successful, calls `FUN_101c4ab0` to set module COM interface

**Parameters** (`__thiscall`):
- ECX = warehouse pointer (from `GetWarehouse(NULL)`)
- Stack arg 1 = type array (from `FUN_101d43a0(category, outArray)`)
- Stack arg 2 = ProgId (from the operation that just succeeded)
- Stack arg 3 = output int pointer (can be dummy)

**Requirements**:
- Resolve `FUN_101d0950` entry point: from patch site `FUN_101d0950_thread_check_INT3`
  (already applied, `sigMatchRva` gives the function RVA)
- Resolve `FUN_101d43a0` (type array builder): **new signature needed**
- A valid ProgId of an existing item (available from createEntry/findExistPos)

**Risk**: LOW — same function used by deleteEntry, proven to work. The
function is already patched. The only new work is resolving `FUN_101d43a0`.

**Side effects**: Allocates a module object (0x241 bytes) and increments its
refcount. The caller must release the module object (or accept a minor
memory leak per refresh — the module object is small).

### Approach B: Dispatch cmd 0x09 via `FUN_101d16c0`

`FUN_101d16c0` is already INT3-patched. It dispatches cmd 0x09, which sets
`[warehouse+0x5E]` on success.

**Parameters** (`__thiscall`):
- ECX = warehouse pointer
- Stack arg 1 = type code
- Stack arg 2 = subtype/name
- Stack arg 3 = output int pointer

**Risk**: LOW — already patched. But cmd 0x09's COM call (vtable +0x24)
might fail for certain type/name combinations, clearing `[warehouse+0x5E]`.

### Approach C: Dispatch cmd 0x10 via `FUN_101d18c0`

`FUN_101d18c0` dispatches cmd 0x10, which sets `[warehouse+0x5E]` on success.
But it is NOT INT3-patched. A new patch would be needed.

**Risk**: MEDIUM — requires a new patch site. The thread check might pass
(our thread matches `[warehouse+0x76]`), but this is unverified for all
execution paths.

### Approach D: Call `_GetMasterRecByType@16` (uses `FUN_101d15c0`, cmd 0x0D)

`_GetMasterRecByType@16` is a named export. It calls `FUN_101cfe00` which in
local mode calls `FUN_101d15c0` (cmd 0x0D). But in server mode, it takes the
BST cache branch and does NOT dispatch cmd 0x0D. And `FUN_101d15c0` is NOT
patched.

**Risk**: HIGH — server mode bypasses the dispatch, and a new patch is
needed for the local mode path. Not recommended.

### Approach E: Manual COM Acquisition

Manually obtain the IWHouse interface by calling `QueryInterface` on the COM
server (`[warehouse+0xAE]`) and storing the result in `[warehouse+0x5E]`.

**Risk**: HIGH — requires understanding the COM server's interface GUID and
the QueryInterface protocol. Fragile and error-prone.

### Approach F: `FUN_101a8620(NULL path)`

`FUN_101a8620` calls `GetWarehouse(path)` with a path parameter. If path is
NULL, it uses `GetWarehouse(NULL)` (our warehouse). It then dispatches cmd
0x0F via `FUN_101d0950` (already patched). It also calls `FUN_101c09f0(0xC)`
(cache refresh type 0xC) — which dispatches cmd 0x4E internally.

**However**: `FUN_101a8620` has a guard: `if (DAT_1023aaac != 1)`. If
`DAT_1023aaac == 1`, the entire function is skipped. The value of
`DAT_1023aaac` is unknown without runtime tracing.

**Risk**: MEDIUM — depends on `DAT_1023aaac`. Also has side effects (cache
refresh, module processing). The function address is not an export and
would need a new signature.

### Approach G: Do Nothing (Remove Cache Refresh)

Accept that `[warehouse+0x5E]` is NULL and remove the `refreshCache()` call.
The BST cache is updated by the warehouse's own mechanisms (e.g., during
save/verify). New items created in the current session ARE visible to
`findExistPos` (which uses the BST cache, not the COM server directly).

**Risk**: LOW for correctness, but the BST cache may be stale for items
created by other processes. The `enumerateRecords` function (which calls
`refreshCache()` before `_LoadMasterRecsEX@16`) would use whatever state
the BST cache has, which may not reflect external changes.

---

## 8. Recommended Approach: A (Dispatch cmd 0x0F via `FUN_101d0950`)

### 8.1 Rationale

1. `FUN_101d0950` is already INT3-patched — no new patches needed
2. It's the same function used by `deleteEntry` — proven to work
3. It sets `[warehouse+0x5E]` on success — directly solving the problem
4. Low risk: the function is well-understood and already exercised

### 8.2 Implementation Plan

1. **Resolve `FUN_101d0950` address**: The patch site
   `FUN_101d0950_thread_check_INT3` already provides `sigMatchRva`. The
   function entry point is `baseAddress + sigMatchRva`. Store this as
   `m_pCmd0FDispatcher` (or similar).

2. **Resolve `FUN_101d43a0` address**: This internal function builds a type
   array from a category code. A new signature is needed. The function is
   called by `FindExistPos`, `_CreateEntryEX@24`, `_SaveModule_New@20`, and
   `_VerifyModule_New@20`, so its pattern is well-established.

3. **Call sequence** (in `refreshCache()`, before the cmd 0x4E dispatch):
   ```cpp
   // 1. If [warehouse+0x5E] is already set, skip initialization
   void* comInterface = *(void**)((char*)warehouse + 0x5E);
   if (comInterface) goto dispatch_0x4E;

   // 2. Build type array from a known category (e.g., 0 = Strategy)
   int typeArray[16];
   m_pBuildTypeArray(category, typeArray);  // FUN_101d43a0

   // 3. Dispatch cmd 0x0F with a known existing ProgId
   int outParam = 0;
   // __thiscall: ECX=warehouse, stack=typeArray, progId, &outParam
   callCmd0F(m_pCmd0FDispatcher, warehouse, typeArray, progId, &outParam);

   // 4. Check if [warehouse+0x5E] is now set
   comInterface = *(void**)((char*)warehouse + 0x5E);
   if (!comInterface) return;  // initialization failed — skip refresh

   dispatch_0x4E:
   // 5. Set type code and dispatch cmd 0x4E (existing code)
   *(int*)((char*)warehouse + 0x12) = 0xC;
   callCmdDispatcher(m_pCmdDispatcher, warehouse, 0x4E);
   ```

4. **ProgId source**: Use the ProgId from the just-completed operation
   (available in `doInsert`/`doModify`/`doDelete`). For `enumerateRecords`
   (which calls `refreshCache()` without a specific ProgId), use
   `findExistPos` with a known item, or call `_LoadMasterRecsEX@16` first to
   find any existing item.

### 8.3 Safety Notes

- **SEH protection**: Wrap the cmd 0x0F dispatch in `__try/__except`, as
  with all other orlib20 calls.
- **Thread safety**: `FUN_101d0950` acquires the warehouse mutex, signals
  the worker, and waits for completion. This is thread-safe.
- **Memory**: `FUN_101d0950` allocates a module object on success. The
  caller should release it (via `(**(code **)*puVar3)(1)` — a Release call).
  If the release is omitted, there is a minor memory leak per refresh
  (0x241 bytes). This is acceptable for a once-per-operation refresh.
- **Idempotency**: If `[warehouse+0x5E]` is already set, the cmd 0x0F
  dispatch will overwrite it (the worker calls `FUN_101cf020` which does a
  fresh QueryInterface). This is safe — the old interface is released by
  the worker before setting the new one.
- **Failure handling**: If cmd 0x0F fails (COM server call returns
  non-zero), `[warehouse+0x5E]` is set to 0 by the failure path. The
  refreshCache function should check `[warehouse+0x5E]` after the cmd 0x0F
  dispatch and skip the cmd 0x4E dispatch if it's still NULL.

### 8.4 Complexity Assessment

| Aspect | Complexity |
|--------|------------|
| New patches | 0 (FUN_101d0950 already patched) |
| New signatures | 1 (FUN_101d43a0 type-array builder) |
| New exports | 0 |
| Inline asm | 1 (__thiscall invocation, same as callCmdDispatcher) |
| New code | ~20-30 lines |
| Risk | LOW |

---

## 9. Correction of Prior Document

The following claims in `docs/RE/archive/RE_F5_cache_refresh_feasibility.md` are
**incorrect** and should be disregarded:

| Section | Claim | Correction |
|---------|-------|------------|
| §4.1 table | Cmd 0x0E writes `[warehouse+0x5E]` | **WRONG**: cmd 0x0E does NOT write it |
| §5.1.2 | "Cmd 0x0E SETS `[warehouse+0x5E]` on success" | **WRONG**: cmd 0x0E only stores status at `[warehouse+0x4E]` |
| §5.1.3 | "After saveSource: `[warehouse+0x5E]` IS SET" | **WRONG**: saveSource dispatches cmd 0x0E which does NOT set it |
| §5.2 | "After MODIFY: `[warehouse+0x5E]` IS SET" | **WRONG**: modify dispatches cmd 0x0E which does NOT set it |
| §5.1.5 | "createEntry: cmd 0x09 + cmd 0x0E → YES" | **PARTIALLY WRONG**: cmd 0x09 may set it, but cmd 0x0E does NOT |
| §1 | "After every INSERT, MODIFY, or DELETE, `[warehouse+0x5E]` is guaranteed to be set" | **WRONG**: only DELETE reliably sets it; INSERT is conditional; MODIFY does NOT |

The correct picture:
- **DELETE**: Sets `[warehouse+0x5E]` via cmd 0x0F (reliable)
- **INSERT**: May set `[warehouse+0x5E]` via cmd 0x09 during createEntry (conditional, unreliable)
- **MODIFY**: Does NOT set `[warehouse+0x5E]` (cmd 0x0E does not write it)
- **EXISTS/GET-SOURCE**: Does NOT set `[warehouse+0x5E]`

---

## 10. Open Questions

1. **Why does cmd 0x09 during createEntry not leave `[warehouse+0x5E]` set?**
   Static analysis shows it should (if the COM call succeeds). Runtime
   tracing (Frida) of `[warehouse+0x5E]` during createEntry would clarify
   whether the COM call fails, `FUN_101cf020` throws, or something else
   clears the field.

2. **What is `DAT_1023aaac`?** This global controls whether `FUN_101a8620`
   executes. If it's 0, Approach F becomes viable (and would handle both
   cmd 0x0F dispatch and cmd 0x4E cache refresh in one call).

3. **Is `[warehouse+0xC2]` always 1 for our warehouse?** The initializer
   sets it to 1 when `DAT_1023aa98 == 0`. If `DAT_1023aa98` is non-zero,
   the warehouse would be in local mode, changing the entire analysis.
