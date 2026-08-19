# RE F5: TSDev Open Dialog Enumeration Findings

## 1. Assessment Verdict

**MAYBE** — The TSDev Open dialog (`CELFileOpenDialog`, implemented in
`ORELUI20.dll`) uses the **same cached command 0x08** path as
`_LoadMasterRecsEX@16`. It does **not** use a different, live enumeration
API. Two alternative live enumeration exports (`_GetMasterRecsBuf@12`,
`_GetMasterRecsPtr@8`) exist in orlib20.dll but are **not imported** by
TSDev.exe or ORELUI20.dll, and are **not invoked** by the Open dialog.

The dialog's `LoadList` method dispatches command 0x08
(`IWHouse::ProgramsList`) via a direct warehouse access path in
ORELUI20.dll. Whether this sees session-created items depends on whether
WHServer's `ProgramsList` returns fresh data — this requires **Frida
live tracing** to confirm.

---

## 2. Exact Dialog Enumeration Call Chain

### 2.1 Dialog class and location

`CELFileOpenDialog` is implemented in **`ORELUI20.dll`** (not TSDev.exe,
not orlib20.dll). TSDev.exe imports the constructor and methods from
ORELUI20.dll by name.

**TSDev.exe call sites** (2 constructor invocations):

| Caller | Address | Purpose |
|--------|---------|---------|
| `FUN_004938c0` | `0x00493909` | File→Open handler (standard) |
| `FUN_004ab920` | `0x004ab992` | Alternate open handler (with ProgId retrieval) |

Both create a `CELFileOpenDialog`, call `CDialog::DoModal()`, then
iterate selected items via `GetSelectedCount` / `GetSelectedType` /
`GetSelectedItem` / `GetSelectedProgID`. The list is populated
**inside** the dialog (in ORELUI20.dll), not by TSDev.exe.

### 2.2 Dialog initialization and list population

```
CELFileOpenDialog::OnInitDialog (ORELUI20.dll @ 0x10012F80)
  ├── InitToolbar()
  ├── InitImageList()
  ├── BeginWaitCursor()                    // vtable +0xC0, NOT a cache refresh
  ├── InitLists()                          // ← populates the list
  │     └── for each analysis type (0..12):
  │           ├── CELSecurity::IsTechniqueEnabled()
  │           ├── new CAnalysisTypeInfo()
  │           └── LoadList(this, type, info)
  │                 ├── Ordinal_10(0)                    // GetWarehouse(NULL)
  │                 ├── FUN_10074470(warehouse, 0, appList)   // "all types" path
  │                 │     └── FUN_10074a20(0, typeArray)      // expand types
  │                 │     └── for each expanded type:
  │                 │           └── FUN_10074310(warehouse, type, subList)
  │                 │                 ├── [warehouse+0x12] = type
  │                 │                 ├── WaitForSingleObject(mutex)
  │                 │                 ├── [warehouse+4] = 8       // command 0x08
  │                 │                 ├── SetEvent()              // signal worker
  │                 │                 ├── WaitForSingleObject()   // wait completion
  │                 │                 ├── SafeArray = [warehouse+0x26]
  │                 │                 ├── SafeArrayAccessData()
  │                 │                 ├── FUN_100735e0()          // copy to list
  │                 │                 └── SafeArrayDestroy()
  │                 └── for each item in appList:
  │                       └── new CELTechInfo()
  ├── EndWaitCursor()
  └── RestoreDlgState()
```

**Key evidence** — `FUN_10074310` decompilation excerpt (ORELUI20.dll @
`0x10074310`):

```c
*(LONG *)((int)this + 0x12) = param_1;       // [warehouse+0x12] = type
WaitForSingleObject(*(HANDLE *)((int)this + 0x72), 0xffffffff);
// ... thread check INT3 ...
*(undefined4 *)((int)this + 4) = 8;          // command 0x08
SetEvent(*(HANDLE *)((int)this + 0x6e));
WaitForSingleObject(*(HANDLE *)((int)this + 0x6a), 0xffffffff);
ReleaseMutex(*(HANDLE *)((int)this + 0x72));
// ... read SafeArray from [warehouse+0x26], copy data ...
```

### 2.3 Category dropdown handler

`OnSelchangeAnalysisType` (ORELUI20.dll @ `0x10013800`) does **not**
re-query the warehouse. It calls `UpdateListCtrl(this, info)` which
iterates the already-loaded `CMapWordToPtr` entries (populated by
`LoadList`) and inserts them into the `CListCtrl`. No orlib20 calls are
made during category switching.

### 2.4 Type expansion in the dialog

`FUN_10074a20` (ORELUI20.dll @ `0x10074A20`) expands categories to
warehouse types:

| Category | Expanded types |
|----------|---------------|
| 0 (Strategy) | `0, 0x200` |
| 1 (Function) | `1` |
| 2 (Indicator) | `2, 0x502, 0x402, 0x602, 0x802, 0xA02, 0xC02, 0xE02, 0x1002, 0x1202, 0x1402` |
| 3 (ActivityBar) | `3` |
| 4 (SystemFunction) | `4` |

Note: TradingApp (`0x1402`) is included under category 2 (Indicator),
consistent with the existing `FUN_101d43a0` expansion in orlib20.dll.

---

## 3. Cache Refresh Behavior

### 3.1 No refresh before enumeration

The Open dialog's `OnInitDialog` does **not** call any cache refresh
function before `InitLists`. The `vtable +0xC0` calls before and after
`InitLists` are `BeginWaitCursor(1)` / `EndWaitCursor(0xFFFFFFFF)` — UI
cursor management, not cache operations.

### 3.2 No FlushCache import

Neither TSDev.exe nor ORELUI20.dll imports any function named
`FlushCache` or similar. orlib20.dll does not export a master-record
flush function. The only flush exports are `ELS_FlushStringCache` and
`FlushStringCacheFromEvents` (string caches, unrelated).

### 3.3 IWHouse::FlushCache exists but is unreachable

The WHServer COM interface `IWHouse` has a `FlushCache` method (typelib
index 32, oVft `0x100`). The orlib20 worker thread handler has a case
for **command 0x45** that calls vtable offset `0x80` (oVft `0x100` =
`FlushCache`). However, **no function in orlib20.dll dispatches command
0x45**. A byte-pattern search for `C7 ?? 04 45 00 00 00` (MOV
[reg+4], 0x45) returned zero matches across the entire orlib20.dll
image. The `FlushCache` COM method is unreachable through the existing
orlib20 API.

### 3.4 Command 0x4E (Approach A) remains the only cache-reset path

The existing `docs/RE/archive/RE_F5_cache_fix_findings.md` documents command 0x4E
(dispatched via `FUN_101c09f0`), which performs a `QueryInterface` on
the WHServer COM interface. This is a module-level cache invalidation,
not a master-record refresh. It requires a module object (not the
warehouse), creating the chicken-and-egg problem documented in the
existing findings.

---

## 4. Alternative Enumeration APIs

### 4.1 IWHouse COM interface methods (from typelib)

The WHServer.exe type library defines `IWHouse` with 58 methods. The
typelib reports vtable offsets (`oVft`) in 8-byte units; the actual
32-bit vtable byte offset is `oVft / 2`. The orlib20 worker thread
handler maps commands to vtable offsets.

**Key command-to-method mapping** (verified via typelib + worker thread
decompilation):

| Command | Vtable offset | oVft | IWHouse method | Type |
|---------|--------------|------|----------------|------|
| 0x08 | 0x6C | 0xD8 | `ProgramsList` | Populates BST from SafeArray |
| 0x09 | 0x24 | 0x48 | `ProgramByName` | Live lookup by name |
| 0x0D | 0x20 | 0x40 | `ProgramById` | Live lookup by ProgId |
| 0x0F | 0xA8 | 0x150 | `ProgramByIdTypes` | Live lookup by ProgId + types |
| 0x10 | 0xAC | 0x158 | `ProgramByNameTypes` | Live lookup by name + types |
| 0x03 | 0x70 | 0xE0 | `MasterRecsByType` | Returns SafeArray by type |
| 0x1A | 0x88 | 0x110 | `MasterRecsByTypes` | Returns SafeArray by types |
| 0x45 | 0x80 | 0x100 | `FlushCache` | Cache flush (unreachable) |

### 4.2 `_GetMasterRecsBuf@12` — command 0x1A (MasterRecsByTypes)

| Field | Value |
|-------|-------|
| Export name | `_GetMasterRecsBuf@12` |
| Ordinal | 512 |
| RVA | `0x1A0C70` |
| VA | `0x101A0C70` |
| Calling convention | `__stdcall` (decorated `@12` = 3 params × 4 bytes) |
| Signature | `int __stdcall GetMasterRecsBuf(int category, void **outBuf, int *outCount)` |

**Internal flow**: calls `FUN_101d43a0` (type expansion) then
`FUN_101d01d0` (command 0x1A). The worker thread calls
`IWHouse::MasterRecsByTypes` (vtable 0x88), which returns a SafeArray of
records for the given types. The SafeArray is read from
`[warehouse+0x36]`, copied to a newly allocated buffer, and
post-processed by `FUN_101d4130` (moves ProgId from offset `0x107` to
`0xD0`, writes warehouse pointer at offset `0x06`).

**INT3 patch required**: `FUN_101d01d0` at VA `0x101D020D`, offset 0x3D
(61) from function start `0x101D01D0`. Patch: `CC → 90`.

**Unique byte pattern** (62 bytes, verified 1 match):
```
55 8B EC 51 8B 45 0C 53 8B D9 56 FF 75 08 C7 00
00 00 00 00 B8 00 20 00 00 66 89 43 1E E8 ?? ?? 
?? ?? 8B 35 ?? ?? ?? ?? 6A FF FF 73 72 89 43 26 
FF D6 FF 15 ?? ?? ?? ?? 3B 43 76 74 01 CC FF 73 
6E C7 43 04 1A 00 00 00
```

**Buffer management**: The buffer is allocated internally via
`operator_new__`. The caller must free it via `operator_delete__` (or
`free`). Record size is `0x10A` bytes. The count is stored at
`*outCount`.

### 4.3 `_GetMasterRecsPtr@8` — command 0x03 (MasterRecsByType)

| Field | Value |
|-------|-------|
| Export name | `_GetMasterRecsPtr@8` |
| Ordinal | 514 |
| RVA | `0x1A0BC0` |
| VA | `0x101A0BC0` |
| Calling convention | `__stdcall` (decorated `@8` = 2 params × 4 bytes) |
| Signature | `int __stdcall GetMasterRecsPtr(int category, void *outBuf)` |

**Internal flow**: calls `FUN_101d43a0` (type expansion) then
`FUN_101d00c0` (command 0x03). The worker thread calls
`IWHouse::MasterRecsByType` (vtable 0x70), which returns a SafeArray of
records for a single type. The SafeArray is read from
`[warehouse+0x26]`, copied to the **caller-allocated** buffer, and
post-processed by `FUN_101d4130`.

**INT3 patch required**: `FUN_101d00c0` at VA `0x101D00EF`, offset 0x2F
(47) from function start `0x101D00C0`. Patch: `CC → 90`.

**Unique byte pattern** (50 bytes, verified 1 match):
```
55 8B EC 51 8B 45 0C 56 8B 35 ?? ?? ?? ?? 57 8B 
F9 C7 00 00 00 00 00 8B 45 08 6A FF FF 77 72 89 
47 12 FF D6 FF 15 ?? ?? ?? ?? 3B 47 76 74 01 CC 
FF 77 6E C7 47 04 03 00 00 00
```

**Buffer management**: The caller must pre-allocate the buffer. Record
size is `0x10A` bytes. The count is returned as the function result.

### 4.4 `_GetMasterRecsEX@8` — thin wrapper

`_GetMasterRecsEX@8` (ordinal 513, RVA `0x107B30`) is a thin wrapper
that calls `_GetMasterRecsPtr@8`. ORELUI20.dll imports this, but it is
only used by `OnVerifyAll`, not by the Open dialog.

### 4.5 Neither alternative is imported by TSDev.exe or used by the dialog

**TSDev.exe orlib20 imports** (from PE import table, via `pefile`):

TSDev.exe imports `_GetNumofMasterRecsEX@4` (IAT `0x560260`) but does
**not** import `_GetMasterRecsBuf@12`, `_GetMasterRecsPtr@8`, or
`_GetMasterRecsEX@8`.

**ORELUI20.dll orlib20 imports**:

ORELUI20.dll imports `_LoadMasterRecsEX@16`, `_LoadMasterRecsEXOld@16`,
`_GetMasterRecsEX@8`, `_GetNumofMasterRecsEX@4`, and ordinals 28, 33,
48, 49, 56, 57, 59, 70, 71, 72. It does **not** import
`_GetMasterRecsBuf@12` or `_GetMasterRecsPtr@8`.

`_GetMasterRecsEX@8` (= `_GetMasterRecsPtr@8`, command 0x03) is only
called from `OnVerifyAll` (ORELUI20.dll @ `0x1004642D`), not from the
Open dialog.

---

## 5. Comparison with Approach A and Approach B

### 5.1 vs. Approach A (`_LoadMasterRecsEX@16`, command 0x08)

The Open dialog uses the **same command 0x08** as Approach A, via a
direct warehouse access path in ORELUI20.dll (`FUN_10074310`). The
dialog does not use `_LoadMasterRecsEX@16` directly — it calls the
command 0x08 dispatcher inline. Both paths:

- Call `IWHouse::ProgramsList(type)` via the worker thread
- Receive a SafeArray at `[warehouse+0x26]`
- Populate a BST from the SafeArray
- Iterate the BST for item data

**The dialog and Approach A share the same data source and the same
staleness characteristics.**

### 5.2 vs. Approach B (`_GetMasterRecByType@16`, command 0x0D)

Approach B uses command 0x0D (`IWHouse::ProgramById`), a live lookup by
ProgId. The Open dialog does not use this for enumeration. The dialog's
`Ordinal_28` (orlib20 ordinal 28, RVA `0x1A03F0`) does use command 0x0F
(`ProgramByIdTypes`) for individual record retrieval after enumeration,
but the enumeration itself (count + ProgId list) uses cached command
0x08.

### 5.3 vs. Alternative APIs (commands 0x03 and 0x1A)

`_GetMasterRecsBuf@12` (command 0x1A, `MasterRecsByTypes`) and
`_GetMasterRecsPtr@8` (command 0x03, `MasterRecsByType`) call different
COM methods that return SafeArrays of records directly. These are
**distinct from command 0x08** (`ProgramsList`):

| Feature | Command 0x08 (ProgramsList) | Command 0x03 (MasterRecsByType) | Command 0x1A (MasterRecsByTypes) |
|---------|---------------------------|-------------------------------|--------------------------------|
| Returns | BST-populating SafeArray | Record SafeArray (copy) | Record SafeArray (copy) |
| Post-processing | BST insertion | `FUN_101d4130` record fixup | `FUN_101d4130` record fixup |
| Multi-type | No (single type per call) | No (single type) | Yes (type array) |
| Export wrapper | `_LoadMasterRecsEX@16` | `_GetMasterRecsPtr@8` | `_GetMasterRecsBuf@12` |
| INT3 offset | Already patched | 0x2F (47) | 0x3D (61) |
| Used by Open dialog | **Yes** | No | No |

**Whether commands 0x03/0x1A return fresher data than 0x08 is
unconfirmed.** All three call WHServer COM methods. If WHServer's
`ProgramsList` and `MasterRecsByType(s)` read from the same server-side
state, they would have the same freshness. If `MasterRecsByType(s)`
bypasses a server-side cache that `ProgramsList` uses, they could be
fresher. **This requires Frida confirmation.**

---

## 6. Implementation Guidance

### 6.1 If commands 0x03/0x1A prove to be live (Frida-confirmed)

`_GetMasterRecsBuf@12` (command 0x1A) is the recommended enumeration
API because:

1. It accepts a **type array** (multi-subtype enumeration in one call).
2. It allocates the buffer internally (simpler caller code).
3. It returns records in the same `0x10A`-byte format as
   `_LoadMasterRecsEX@16`.
4. It requires only **one new INT3 patch** (`FUN_101d01d0`, offset 0x3D).

**Usage pattern**:
```c
// Pseudo-code (NOT production — for guidance only)
void *buf = NULL;
int count = 0;
int category = 2;  // Indicator (includes TradingApp via 0x1402)
if (_GetMasterRecsBuf(category, &buf, &count) && count > 0) {
    for (int i = 0; i < count; i++) {
        char *record = (char *)buf + i * 0x10A;
        // Name at record + 0x00 (null-terminated string)
        // ProgId at record + 0xD0 (after FUN_101d4130 post-processing)
        // Subtype at record + 0xE9
    }
    operator_delete(buf);  // or free(buf)
}
```

**Required patches**:
- `FUN_101d01d0` INT3 at offset 0x3D: `CC → 90` (new patch)
- Existing patches for commands 0x0F, 0x1E, 0x17, 0x11 remain required
- Patch restoration bookkeeping must be updated (array size + destructor
  loop) per the adversarial review corrections in
  `docs/RE/archive/RE_F5_cache_fix_review.md`

### 6.2 If commands 0x03/0x1A are also cached

If Frida tracing shows that `MasterRecsByTypes` also returns cached data,
then no orlib20-based enumeration API can see session-created items. In
that case, the only options are:

1. **Call `IWHouse::FlushCache` directly** — requires dispatching
   command 0x45, which no orlib20 function does. Would require a custom
   command dispatcher or direct COM vtable call.
2. **Re-initialize the warehouse** — call `OrlibExit` + `OrlibInit`,
   which re-populates caches from WHServer. Heavy-weight but proven.
3. **Accept the limitation** — enumerate at startup before any items are
   created, or re-initialize before enumeration.

---

## 7. Risks and Unknowns

### 7.1 Requires Frida confirmation (CRITICAL)

The most important unknown is whether `IWHouse::ProgramsList` (command
0x08) and `IWHouse::MasterRecsByTypes` (command 0x1A) return fresh data
including session-created items. Static analysis confirms both are COM
calls to WHServer, but WHServer may have its own server-side cache.

**Frida test procedure**:
1. Launch TSDev.exe with Frida attached.
2. Before opening the dialog, create a new item via the automation CLI.
3. Open the File→Open dialog and check if the new item appears.
4. If it does not appear, call `_GetMasterRecsBuf@12` via Frida and
   check if the new item is in the returned records.
5. If neither sees the item, call `IWHouse::FlushCache` (vtable 0x80)
   via Frida, then retry both.

**UI interaction note**: Opening the File→Open dialog requires user
interaction (clicking File→Open in the TSDev UI). Frida can hook the
relevant functions but cannot automate the UI click. A human operator
or UI automation tool is needed.

### 7.2 INT3 thread-check patches

Both `_GetMasterRecsBuf@12` and `_GetMasterRecsPtr@8` have INT3
thread-check instructions in their internal command dispatchers. These
must be patched (`CC → 90`) before calling from a non-worker thread.
The byte patterns are unique (verified by Ghidra search: 1 match each).

### 7.3 Record layout assumptions

The record layout (`0x10A` bytes) is assumed to be the same as
`_LoadMasterRecsEX@16`'s output. The `FUN_101d4130` post-processing
function moves the ProgId from offset `0x107` to `0xD0` and clears
`0x107`. This is the same post-processing applied by
`_LoadMasterRecsEX@16`. The name is at offset `0x00` (null-terminated).
The subtype is at offset `0xE9`.

### 7.4 Type expansion differences

`_GetMasterRecsBuf@12` uses `FUN_101d43a0` (orlib20.dll) for type
expansion, which is the same function used by `_LoadMasterRecsEX@16`
and `FindExistPos`. The Open dialog uses `FUN_10074a20` (ORELUI20.dll)
which has a slightly different expansion (e.g., Strategy includes both
`0` and `0x200`). The orlib20 expansion is authoritative for the
project's use case.

### 7.5 `_GetMasterRecsBuf@12` parameter order

The decompilation of `_GetMasterRecsBuf@12` shows 3 stack parameters
(`RET 0xC`): `category`, `outBuf` (pointer to receive buffer pointer),
and `outCount` (pointer to receive count). The disassembly confirms:

```asm
PUSH [EBP+0x10]    ; outCount (3rd param)
LEA EAX, [EBP-0x3c] ; type array (local)
MOV ECX, ESI       ; warehouse (this)
PUSH EAX           ; type array
CALL FUN_101d01d0  ; returns buffer pointer in EAX
MOV [ECX], EAX     ; *outBuf = buffer pointer  (ECX = [EBP+0xC] = outBuf)
```

### 7.6 Worker thread command 0x45 (FlushCache) is unreachable

No function in orlib20.dll sets command 0x45. The byte pattern
`C7 ?? 04 45 00 00 00` (MOV [reg+4], 0x45) returns zero matches. The
`FlushCache` method exists on the `IWHouse` COM interface but cannot be
invoked through any existing orlib20 export or internal function. To
use it, one would need to either:
- Write a custom command dispatcher that sets `[warehouse+4] = 0x45`
  and signals the worker thread event, or
- Obtain the `IWHouse` COM interface pointer directly and call
  `FlushCache` via the vtable.

---

## 8. Summary of Confirmed Facts

| Claim | Evidence | Status |
|-------|----------|--------|
| Open dialog is `CELFileOpenDialog` in ORELUI20.dll | PE import table: TSDev.exe imports constructor from ORELUI20.dll | **Confirmed** |
| Dialog uses command 0x08 for enumeration | `FUN_10074310` decompilation: `[warehouse+4] = 8` | **Confirmed** |
| Command 0x08 = `IWHouse::ProgramsList` | Worker thread handler: vtable 0x6C; typelib: oVft 0xD8 = ProgramsList | **Confirmed** |
| Dialog does NOT use `_GetMasterRecsBuf@12` or `_GetMasterRecsPtr@8` | PE import tables: neither imported by TSDev.exe or ORELUI20.dll | **Confirmed** |
| Dialog does NOT call FlushCache before enumeration | `OnInitDialog` decompilation: no flush/refresh calls | **Confirmed** |
| `IWHouse::FlushCache` exists (command 0x45) | Worker thread handler case 0x45 → vtable 0x80; typelib: FlushCache | **Confirmed** |
| Command 0x45 is unreachable from orlib20 exports | Byte pattern search: 0 matches for command 0x45 dispatch | **Confirmed** |
| Command 0x1A = `IWHouse::MasterRecsByTypes` | Worker thread handler: vtable 0x88; typelib: oVft 0x110 = MasterRecsByTypes | **Confirmed** |
| Command 0x03 = `IWHouse::MasterRecsByType` | Worker thread handler: vtable 0x70; typelib: oVft 0xE0 = MasterRecsByType | **Confirmed** |
| Commands 0x03/0x1A return fresh data | Both call WHServer COM methods | **Likely, unconfirmed** |
| Commands 0x03/0x1A see session-created items | Not tested | **Unconfirmed — requires Frida** |
| Command 0x08 sees session-created items | Not tested | **Unconfirmed — requires Frida** |
| `_GetMasterRecsBuf@12` INT3 at offset 0x3D | Disassembly of `FUN_101d01d0` | **Confirmed** |
| `_GetMasterRecsPtr@8` INT3 at offset 0x2F | Disassembly of `FUN_101d00c0` | **Confirmed** |
| Both INT3 byte patterns are unique | Ghidra byte-pattern search: 1 match each | **Confirmed** |

---

## 9. Binaries and Tools Used

| Binary | Source | Ghidra program |
|--------|--------|----------------|
| TSDev.exe | `C:\Program Files (x86)\TradeStation 9.5\Program\TSDev.exe` | `/TSDev.exe.0` |
| orlib20.dll | `C:\Program Files (x86)\TradeStation 9.5\Program\orlib20.dll` | `/orlib20.dll` |
| WHServer.exe | `C:\Program Files (x86)\TradeStation 9.5\Program\WHServer.exe` | `/WHServer.exe` |
| ORELUI20.dll | Copied to `tmp/ORELUI20.dll` from TS install | `/ORELUI20.dll` (newly imported) |

**ORELUI20.dll was newly imported into the Ghidra project during this
investigation.** It was not previously analyzed.

**WHServer.exe typelib** was parsed using `pythoncom.LoadTypeLib()` to
extract the `IWHouse` interface method names and vtable offsets. The
typelib reports vtable offsets in 8-byte units (`oVft`); the actual
32-bit vtable byte offset is `oVft / 2`. This mapping was validated
against the worker thread handler's vtable calls:

- Command 0x0D → vtable 0x20 → oVft 0x40 → `ProgramById` ✓
- Command 0x09 → vtable 0x24 → oVft 0x48 → `ProgramByName` ✓
- Command 0x08 → vtable 0x6C → oVft 0xD8 → `ProgramsList` ✓
- Command 0x03 → vtable 0x70 → oVft 0xE0 → `MasterRecsByType` ✓
- Command 0x1A → vtable 0x88 → oVft 0x110 → `MasterRecsByTypes` ✓
- Command 0x45 → vtable 0x80 → oVft 0x100 → `FlushCache` ✓
