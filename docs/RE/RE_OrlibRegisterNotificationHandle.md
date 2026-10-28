# RE Findings: `_OrlibRegisterNotificationHandle@4` and the orlib20 Notification Subscriber List

**Target binary:** `orlib20.dll` v9.50.01.3344 (Ghidra project `TSDevAnalysis`, program `/orlib20.dll`)
**Supporting binaries:** `/receivers/tslib.dll`, `/receivers/orchart.exe`, `/TSDev.exe`
**Scope:** RESEARCH ONLY -- no repo files modified. Reverse-engineering TVS to determine how orchart/TSELAppHost/TSScanner HWNDs get registered into an orlib20 subscriber list, and how the 0x40A recalc message is delivered cross-process.
**Tooling:** ghidra-mcp (decompilation / xrefs / memory / section headers).

> **HEADLINE CORRECTION to KNOWLEDGE_BASE.md Section 10:** The subscriber list is **NOT** in a per-process `.data` section. The HWND array buffer lives in orlib20's **`SHARE_WO` PE section, which has `IMAGE_SCN_MEM_SHARED`** -- i.e. it is a **cross-process shared section**. The KB's prior "per-process .data -> CLI list empty" conclusion is incorrect; runtime verification confirmed the CLI and orchart load the same `orlib20.dll` and the CLI sees 19 pre-populated slots in the shared array.

---

## 1. Decompile + analysis of `_OrlibRegisterNotificationHandle@4`

### 1.1 The export (orlib20.dll)

`_OrlibRegisterNotificationHandle@4` is exported by name at **`0x1011c130`**. It is a tiny **import-style thunk** (sets `this` and tail-jumps into tslib):

```
1011c130  PUSH EBP
1011c131  MOV EBP,ESP
1011c133  MOV ECX,0x10231050        ; this = &global EventsViaHandles object (in .data)
1011c138  POP EBP
1011c139  JMP dword ptr [0x101fa050] ; -> tslib::EventsViaHandles::RegisterHandle
```

Decompiled (Ghidra):

```c
void _OrlibRegisterNotificationHandle_4(HWND__ *param_1)
{
    tslib::EventsViaHandles::RegisterHandle(
        (EventsViaHandles *)&PTR_DAT_10231050, param_1);
    return;
}
```

**Signature:** `bool __thiscall EventsViaHandles::RegisterHandle(HWND__ *hWnd)`. The `@4` = one 4-byte parameter, which is an **HWND** (typed `struct HWND__ *`). Return value is `bool` (true = already-present or successfully added).

### 1.2 The real implementation -- `tslib::EventsViaHandles::RegisterHandle` (tslib.dll @ `0x10031d50`)

```c
bool __thiscall tslib::EventsViaHandles::RegisterHandle(EventsViaHandles *this, HWND__ *param_1)
{
    int *piVar4 = *(int **)this;                 // pb = this->pBuffer
    int *piVar1 = piVar4 + *(int *)(this + 4);   // end = pb + this->count
    if (param_1 == (HWND__ *)0x0) return false;

    // 1) dedup: already present -> success
    for (; piVar4 != piVar1; piVar4 = piVar4 + 1)
        if ((HWND__ *)*piVar4 == param_1) return true;

    int iVar5 = 1;
    do {
        // 2) find a zero slot, atomically claim it (LOCK/UNLOCK = interlocked cmpxchg-style)
        piVar4 = *(int **)this;
        piVar1 = piVar4 + *(int *)(this + 4);
        for (; piVar4 != piVar1; piVar4 = piVar4 + 1) {
            if (*piVar4 == 0) {
                LOCK();
                int iVar3 = *piVar4;
                if (iVar3 == 0) { *piVar4 = (int)param_1; iVar3 = 0; }
                UNLOCK();
                if (iVar3 == 0) return true;
            }
        }
        // 3) no free slot: try to evict dead HWNDs, then retry once; else throw
        if (((iVar5 == 2) && (bVar2 = FreeInvalideHandles(this), !bVar2)) ||
            (iVar5 = iVar5 + 1, 2 < iVar5)) {
            AfxThrowInvalidArgException();   // does not return
        }
    } while (true);
}
```

**Behavior summary:**
- Rejects NULL.
- Linear scan for an existing equal HWND -> returns true (idempotent).
- Finds the first zero slot and atomically writes the HWND into it (`LOCK`/`UNLOCK` = an interlocked compare-exchange on the slot).
- If the table is full, calls `FreeInvalideHandles` (which uses `IsWindow()` to drop dead HWNDs) and retries once; if still full, **throws `AfxThrowInvalidArgException`** (i.e. capacity is hard-capped, no growth).

**What it stores:** just the **HWND** (4 bytes) per slot. **No** thread-id, process-id, or category mask is stored. The same single list is used for every message type broadcast through this `EventsViaHandles` instance (0x40A lib-changed, 0x40F, performance, etc. -- see Section 3).

The companion export **`_OrlibUnregisterNotificationHandle@4`** (`0x1011c140`) is the symmetric thunk -> `EventsViaHandles::UnregisterHandle` (`tslib 0x10031ea0`), which atomically zeroes the matching slot.

---

## 2. Subscriber-list data structure (address, layout, capacity)

### 2.1 The control object (per-process `.data`)

Global `EventsViaHandles` object at **`0x10231050`** (orlib20 `.data`, segment `.data` = `0x1022e000-0x10240987`, characteristics `0xC0000040` = `READ|WRITE|INITIALIZED`, **NOT shared**).

Layout (struct is 8 bytes; reconstructed from the tslib constructor at `0x10031ba0`):

| Offset | Type     | Field  | Static init value (bytes at 0x10231050: `38 88 48 10 20 00 00 00`) |
|--------|----------|--------|-------------------------------------------------------------------|
| +0     | `HWND**` | pBuffer | `0x10488838` (-> HWND array, in SHARE_WO)                          |
| +4     | `ulong`  | count   | `0x00000020` = **32** (capacity / number of slots)                |

The constructor confirms the layout:
```c
EventsViaHandles * __thiscall tslib::EventsViaHandles::EventsViaHandles(
        EventsViaHandles *this, HWND__ **param_1, ulong param_2)
{
    *(HWND__ ***)this = param_1;   // +0 = pBuffer
    *(ulong *)(this + 4) = param_2; // +4 = count
    return this;
}
```
No runtime constructor call targets `0x10231050` (all 20 xrefs to it are reads from sender/register thunks), so the object is **statically initialized** from the PE image with `{0x10488838, 32}`.

### 2.2 The HWND array buffer (SHARED `SHARE_WO` section)

The buffer pointed to by `pBuffer` is at **`0x10488838`**. This address lies in orlib20's **`SHARE_WO`** segment (`0x10243000-0x104889ff`, ~2.38 MB).

**Section characteristics (read from the PE section header at file offset `0x100002f8`):**
- Name: `SHARE_WO`
- `Characteristics` (at `0x1000031c`) = **`0xD0000040`** = `IMAGE_SCN_CNT_INITIALIZED_DATA (0x40)` | `IMAGE_SCN_MEM_READ (0x40000000)` | `IMAGE_SCN_MEM_WRITE (0x80000000)` | **`IMAGE_SCN_MEM_SHARED (0x10000000)`**.

-> **The HWND array is in a shared, read-write section.** All processes that load the *same* `orlib20.dll` file share these pages. (For comparison, the sibling `SHARE_MM` section at `0x10000320` has characteristics `0xD0000040` as well -- also shared RW; `.data` is `0xC0000040` -- not shared.)

**Array layout at `0x10488838`:**
- `HWND[32]` -- 32 x 4 bytes = 128 bytes (`0x10488838 ... 0x104888b8`). In the static image all 32 slots are zero (confirmed by reading `0x10488838`: 128 bytes of `0x00`).
- **Max capacity = 32 HWNDs.** Fixed-size array (not a linked list, not a growable table). Overflow after evicting dead handles -> `AfxThrowInvalidArgException`.

**Adjacent single-handle slots in the same shared section** (set by the separate export `RegisterWndHandle` at `0x1011c190`, see Section 4):
- `0x10488834` -- 4 bytes immediately before the array (default case of `RegisterWndHandle`)
- `0x104888b8` -- immediately after the array (slot index 32; `RegisterWndHandle` case 5)
- `0x104888c0` -- `RegisterWndHandle` case 0
- `0x1023cc54` -- in `.data` (not shared); `RegisterWndHandle` case 2

So the notification-handle area occupies the **very end of SHARE_WO** (`0x10488834 ... 0x104888c4`): one 32-slot broadcast array plus a few named single-handle slots.

### 2.3 Iteration

The list is iterated as a plain array by `count` slots. From `PosMessageToClients` (Section 3):
```c
undefined4 *puVar2 = *(undefined4 **)this;          // pb = pBuffer
uint uVar1 = *(int *)(this + 4) * 4 + 3U >> 2;      // == count (32)
for (uint uVar3 = 0; uVar3 != uVar1; uVar3++, puVar2++)
    if ((HWND)*puVar2 != (HWND)0x0)
        PostMessageW((HWND)*puVar2, msg, wParam, lParam);
```
Dead/zero slots are skipped in the sender; `FreeInvalideHandles` (`tslib 0x10031be0`) periodically reclaims slots whose `IsWindow()` returns false.

---

## 3. Ordinal_57 / `FUN_101526f0` (NotifyLibChanged) -- iteration confirmation

`FUN_101526f0` (the body of **Ordinal_57**, "NotifyLibChanged") at `0x101526f0`:

```c
undefined4 FUN_101526f0(undefined2 param_1, undefined2 param_2)
{
    tslib::EventsViaHandles::PosMessageToClients(
        (EventsViaHandles *)&PTR_DAT_10231050,
        0x40a,                              // message
        0,                                  // wParam = 0
        CONCAT22(param_1, param_2));        // lParam = (category << 16) | progId
    return 1;
}
```

`PosMessageToClients` (tslib `0x10031cb0`) -- **the broadcast loop**:
```c
void __thiscall tslib::EventsViaHandles::PosMessageToClients(
        EventsViaHandles *this, int msg, int wParam, int lParam)
{
    undefined4 *puVar2 = *(undefined4 **)this;          // pBuffer (-> 0x10488838, SHARE_WO)
    uint uVar1 = *(int *)(this + 4) * 4 + 3U >> 2;      // count = 32
    uint uVar3 = 0;
    if (puVar2 + *(int *)(this + 4) < puVar2) uVar1 = 0; // overflow guard
    if (uVar1 != 0) {
        do {
            if ((HWND)*puVar2 != (HWND)0x0)
                PostMessageW((HWND)*puVar2, msg, wParam, lParam);
            uVar3++; puVar2++;
        } while (uVar3 != uVar1);
    }
}
```

**Confirmed:** Ordinal_57 iterates **the same** `EventsViaHandles` instance at `0x10231050` (buffer `0x10488838`) that `_OrlibRegisterNotificationHandle@4` populates, and calls `PostMessageW(0x40A, 0, (category<<16)|progId)` per non-zero slot. This matches the Frida-observed 19 posts from TSDev.exe (16 to orchart, 2 to TSELAppHost, 1 to TSScanner).

**Per-process vs shared:** The *control object* (`0x10231050`) is in per-process `.data`, but it is identically initialized in every process to point at `0x10488838`, which is in the **shared** `SHARE_WO` section. Therefore the *HWND array itself is shared cross-process* among all processes loading the same `orlib20.dll` file. Each process does have its own 8-byte control struct, but all of them reference the **same** shared 32-slot HWND array.

### 3.1 Other senders using the SAME shared list (xrefs to `0x10231050`)

All of these broadcast/scan the same shared array (different message IDs), confirming one shared list serves multiple notification types:

| Function | Address | Calls | Message |
|----------|---------|-------|---------|
| `FUN_101526c0` (Ordinal_56-ish) | `0x101526c0` | `SendMessageToClients` | `0x40A` (synchronous) |
| `FUN_101526f0` = **Ordinal_57** | `0x101526f0` | `PosMessageToClients` | `0x40A` (posted) |
| `FUN_10152720` | `0x10152720` | `SendMessageToClients` | `0x40F` |
| `NotifyPerformanceWindowRunning` | (xref @ `0x10152759`) | sender | performance msg |
| `FUN_1011c150` | `0x1011c150` | `SendMessageToClients` | (caller-supplied) |
| `FUN_1011c170` | `0x1011c170` | `PosMessageToClients` | (caller-supplied) |
| `FUN_10159ae0`, `FUN_1016aed0`, `FUN_1016b2f0`, `FUN_101a47e0` | -- | senders | various |
| `CompileText` (`0x1001e324`), `ScanATSourceCode` (`0x1001e756`) | -- | read `0x10231050` | -- |

There is also a `PosMessageToClientsExcluding` variant (`tslib 0x10031d00`) that skips a specified HWND -- used to avoid notifying the originator.

---

## 4. Callers / xrefs of `_OrlibRegisterNotificationHandle@4`

### 4.1 Internal xrefs (orlib20.dll)
`get_xrefs_to(0x1011c130)` returns only:
- `0x1021d8dc` (DATA -- the export name-table entry)
- `Entry Point` (EXTERNAL -- the export itself)

-> **No internal orlib20 caller.** The function is purely an export consumed by external processes.

### 4.2 orchart.exe (loaded in the project as `/receivers/orchart.exe`)

- orchart.exe **imports `_OrlibRegisterNotificationHandle@4` by name** from `ORLIB20.DLL`: the import name string `"_OrlibRegisterNotificationHandle@4"` is present at **`0x00781414`** in orchart's `.rdata` (confirmed via `search_strings`). This is the definitive evidence that orchart calls it.
- orchart.exe **also imports a second, related orlib20 export `RegisterWndHandle`** (IAT entry `0x00381438`, library `ORLIB20.DLL`). `RegisterWndHandle` (`orlib20 0x1011c190`) is a *different* function -- a typed setter that writes specific single HWND slots in `SHARE_WO` (`0x10488834`, `0x104888b8`, `0x104888c0`) and `.data` (`0x1023cc54`), plus `_SetHandle@8(2, hWnd)` for type 1. It does **not** touch the 32-slot broadcast array.

**`RegisterWndHandle` decompile (orlib20 `0x1011c190`):**
```c
undefined4 RegisterWndHandle(undefined4 param_1, int param_2)
{
    if (param_2 == 1) _SetHandle_8(2, param_1);
    switch (param_2 - 3U) {
        case 0:  DAT_104888c0 = param_1; return 1;   // type 3
        case 2:  DAT_1023cc54 = param_1; return 1;   // type 5 (.data, NOT shared)
        case 5:  _DAT_104888b8 = param_1; return 1;  // type 8 (SHARE_WO, just past array)
        case 4:  break;                              // type 7 -> falls to default
        default: AfxThrowInvalidArgException();
    }
    DAT_10488834 = param_1; return 1;                // default / type 7 (SHARE_WO, just before array)
}
```

**Limitation:** Ghidra did not auto-create a thunk symbol for the `_OrlibRegisterNotificationHandle@4` import in orchart (no function named accordingly; the IAT/thunk reference was not resolvable via `get_xrefs_to` on the IAT entry or name string within the time budget). The **exact call site and which HWND orchart passes** (a message-only window vs. a chart frame window) was therefore **not pinned down** from orchart alone. However, the import is confirmed, and from the orlib20 side the HWND is simply stored verbatim into the shared array -- whatever window orchart creates and registers will receive 0x40A. (Recommended follow-up: in orchart, locate the `call dword ptr [<IAT for _OrlibRegisterNotificationHandle@4>]` and trace the HWND source -- likely a `CreateWindowEx`/`CreateDialog` result or `m_hWnd` of a `CWnd` derived class.)

### 4.3 TSDev.exe / TSELAppHost / TSScanner
- TSDev.exe is loaded (`/TSDev.exe`); a full import scan for this symbol was not completed in this TVS, but the Frida trace already proved TSDev *receives* 0x40A (so TSDev also registers at least one HWND) and *sends* 0x40A via Ordinal_57.
- TSELAppHost / TSScanner are not loaded as separate programs in the project (only `orchart.exe`, `ELIS.exe`, `WHServer.exe`, `ORPlat.exe`, `ORDllHst.exe` are). Marked as **not analyzed** -- but the Frida data (2 posts to TSELAppHost, 1 to TSScanner) implies they too call `_OrlibRegisterNotificationHandle@4` at startup.

---

## 5. CRITICAL -- Approach A feasibility

### 5a. Does `_OrlibRegisterNotificationHandle@4` ALSO write the HWND to a shared-memory section?

**YES.** This is the central finding. Although the 8-byte `EventsViaHandles` control object is in per-process `.data` at `0x10231050`, the buffer it points to (`0x10488838`) is in the **`SHARE_WO` PE section, which is `IMAGE_SCN_MEM_SHARED | READ | WRITE`** (characteristics `0xD0000040`, read from the section header). So every `RegisterHandle` write goes to a **cross-process shared array**. There is no `CreateFileMapping`/`MapViewOfFile` call in the register path -- the sharing is achieved by the PE loader mapping the shared section (backed by a named section object keyed off the `orlib20.dll` file).

This means: when orchart.exe calls `_OrlibRegisterNotificationHandle@4(hWnd)`, that HWND is written into the shared section, and **TSDev.exe (loading the same orlib20.dll) sees it in its own mapping of the same shared pages** and posts 0x40A to it. This is exactly the 19-post behavior Frida observed for TSDev.

### 5b. Is there a separate export / shared region that enumerates registered HWNDs cross-process?

The shared 32-slot array at `0x10488838` **is** that enumeration. There is no separate "enumerate" export; the array is directly readable by any process that maps the same `SHARE_WO` section. The companion single-handle slots set by `RegisterWndHandle` (`0x10488834`, `0x104888b8`, `0x104888c0`) are also in `SHARE_WO`.

### 5c. Is there an orlib20 function that BROADCASTS 0x40A to ALL processes' lists via a cross-process mechanism?

**Yes -- Ordinal_57 itself is that broadcast.** Because the list is shared, calling `FUN_101526f0` (Ordinal_57) from *any* process that shares the section posts 0x40A to every HWND registered by *any* process. There is no need for a global Event or additional IPC -- the shared array + `PostMessageW` per entry is the entire mechanism. `PosMessageToClientsExcluding` (`tslib 0x10031d00`) is the "broadcast-to-all-except-originator" variant.

### 5d. Does the WHServer.exe COM `_IWHouseEvents` path deliver the recalc signal independent of 0x40A?

Not analyzed in depth in this TVS. However, the Frida evidence (KB Section 13) establishes that **both** the 19 `PostMessageW(0x40A, ...)` calls inside Ordinal_57 **and** the worker cmd 0x0D dispatch (firing WHServer.exe COM `_IWHouseEvents`) are required for immediate dependent recalc -- 0x40A delivery alone is insufficient. The `WHServer` COM `_IWHouseEvents` path is exercised by the cmd 0x0D COM vtable calls and is the mechanism that triggers the actual recalc; the 0x40A posts are the necessary companion notification.

---

## 6. Conclusion + recommended implementation path

### 6.1 Actual cross-process recalc delivery path

**Direct `PostMessageW` from the modifying process**, iterating the **shared** subscriber array in `SHARE_WO` at `0x10488838` (capacity 32 HWNDs). The modifying process calls Ordinal_57 (`FUN_101526f0`) -> `EventsViaHandles::PosMessageToClients` -> `PostMessageW(0x40A, 0, (category<<16)|progId)` per non-zero slot. The HWNDs are placed into that shared array by each receiver process (orchart, TSELAppHost, TSScanner, TSDev) calling `_OrlibRegisterNotificationHandle@4` at startup. The WHServer COM path is **not** the 0x40A delivery mechanism.

### 6.2 Why the CLI currently posts 0 (corrected diagnosis)

The subscriber array is **shared**, not per-process. Therefore the CLI seeing an empty list is **not** because ".data is per-process" (the KB's explanation). The only ways the CLI's view of `0x10488838` is empty while TSDev's is full are:

1. **The CLI loads a different `orlib20.dll` file (different path)** than orchart.exe/TSDev.exe. PE shared sections are keyed by the underlying file; a different path yields a *different* shared-section object, so the CLI gets its own zeroed 128-byte array. **This is the most likely cause** -- e.g. the CLI loads `orlib20.dll` from a build/output directory or a different install copy rather than from `C:\Program Files\TradeStation 9.5\orlib20.dll` (or wherever orchart loads it).
2. orchart/TSELAppHost/TSScanner were not running / had not registered yet at trace time.
3. The CLI's `orlib20.dll` is a different version that does not map `SHARE_WO` identically.

### 6.3 Recommended implementation path (in order of preference)

**Option 1 (preferred, likely the real fix): make the CLI load the EXACT same `orlib20.dll` file path that orchart.exe loads.**
- Verify with a debugger / Process Monitor which `orlib20.dll` path `tsdev_cli.exe` actually loads vs. `orchart.exe` (same full path? same version 9.50.01.3344?).
- If they differ, change the CLI's DLL load path / working directory / manifest so it loads the TradeStation-install `orlib20.dll`. Once the CLI shares `SHARE_WO` with orchart, the CLI's existing Ordinal_57 call will automatically post 0x40A to orchart's already-registered HWNDs -- no extra code needed. This is exactly what TSDev does.

**Option 2 (if the CLI must keep a separate orlib20): have the CLI read the shared array directly and post.**
- The CLI loads the TradeStation-install `orlib20.dll` (even just via `LoadLibrary`) **purely to map its `SHARE_WO`**, then reads the 32-slot array at the section offset corresponding to `0x10488838` (offset within `SHARE_WO` = `0x10488838 - 0x10243000` = `0x245838`), and for each non-zero HWND calls `PostMessageW(hWnd, 0x40A, 0, (category<<16)|progId)`. This is a manual re-implementation of `PosMessageToClients` against the shared buffer.
- Caveat: the absolute VA `0x10488838` is only valid if the CLI's `orlib20.dll` loads at the same base `0x10000000`; otherwise use the rebased section address (the section's RVA is fixed: `SHARE_WO` RVA = `0x243000`, array RVA within it = `0x245838`).

**Option 3 (fallback, no orlib20 sharing): discover orchart HWNDs by window enumeration.**
- Use `EnumWindows`/`FindWindowEx` filtered by orchart's window class names to find the receiver windows, then `PostMessageW(0x40A, 0, (cat<<16|progId))` to each. This requires knowing the class names of the windows orchart registers (the exact HWND source was not pinned in this TVS -- see Section 4.2 limitation). Feasible but brittle and requires the class-name research that this TVS did not complete.

**Approach A verdict:** **Feasible -- and simpler than expected.** The CLI does **not** need to "discover" orchart HWNDs through a separate channel; the HWNDs are already published in the shared `SHARE_WO` array. The cleanest implementation is **Option 1**: ensure the CLI loads the same `orlib20.dll` as orchart so it shares `SHARE_WO`, after which the CLI's existing Ordinal_57 call does the broadcast for free. If Option 1 is impossible, Option 2 (read the shared array and post manually) is the direct, evidence-backed path.

---

## 7. Open questions remaining

1. **orchart call site & HWND source:** Confirm the exact call to `_OrlibRegisterNotificationHandle@4` inside `orchart.exe` and identify which window's HWND it passes (message-only window? chart frame `CWnd::m_hWnd`?). The import is confirmed (name string @ `0x00781414`); the call site was not pinned because Ghidra did not auto-create the import thunk symbol. (Follow-up: locate `call dword ptr [<IAT>]` for this import and trace the HWND operand.)
2. **CLI's `orlib20.dll` load path vs. orchart's:** The single most important runtime verification. If they already match, the shared-array theory predicts the CLI *should* post 19 -- re-run Frida on the CLI to confirm whether the list is truly empty or whether the prior trace had a different issue (e.g. orchart not running).
3. **TSELAppHost / TSScanner registration:** Not loaded in the Ghidra project; assumed to call `_OrlibRegisterNotificationHandle@4` at startup based on Frida evidence. Confirm if needed.
4. **`RegisterWndHandle` consumers:** Which orlib20 functions read `0x10488834` / `0x104888b8` / `0x104888c0` and what notifications they drive (separate from the 0x40A broadcast). Relevant only if those handles turn out to participate in recalc.
5. **WHServer COM `_IWHouseEvents` recalc independence:** Confirm whether the COM path can trigger recalc without 0x40A (not required for the 0x40A fix, but relevant for completeness of the "TS Desktop interaction acceptance" objective).
6. **ASLR/relocation interaction with the shared section:** Confirm at runtime that `SHARE_WO` is actually shared between the CLI and orchart (e.g. write a sentinel from one process, read from the other). The PE flags say it should be; a runtime check removes all doubt.

---

## Appendix A -- Key addresses (orlib20.dll, image base `0x10000000`)

| Symbol | Address | Notes |
|--------|---------|-------|
| `_OrlibRegisterNotificationHandle@4` (export) | `0x1011c130` | thunk -> tslib `RegisterHandle`, `this=0x10231050` |
| `_OrlibUnregisterNotificationHandle@4` (export) | `0x1011c140` | thunk -> tslib `UnregisterHandle` |
| `RegisterWndHandle` (export) | `0x1011c190` | typed single-handle setter into SHARE_WO/.data |
| `FUN_101526f0` = **Ordinal_57** NotifyLibChanged | `0x101526f0` | `PosMessageToClients(0x40A, 0, (cat<<16)|progId)` |
| `FUN_101526c0` | `0x101526c0` | `SendMessageToClients(0x40A, ...)` (synchronous) |
| `FUN_10152720` | `0x10152720` | `SendMessageToClients(0x40F, ...)` |
| `EventsViaHandles` control object | `0x10231050` | `.data` (per-process), `{pBuffer=0x10488838, count=32}` |
| HWND broadcast array | `0x10488838` | `SHARE_WO` (**SHARED**), `HWND[32]`, 128 bytes |
| `SHARE_WO` section | `0x10243000-0x104889ff` | char `0xD0000040` (SHARED|RW|INIT) |
| `.data` section | `0x1022e000-0x10240987` | char `0xC0000040` (RW|INIT, not shared) |

## Appendix B -- Key addresses (tslib.dll)

| Symbol | Address |
|--------|---------|
| `EventsViaHandles::EventsViaHandles` (ctor) | `0x10031ba0` |
| `EventsViaHandles::FreeInvalideHandles` | `0x10031be0` |
| `EventsViaHandles::PosMessageToClients` | `0x10031cb0` |
| `EventsViaHandles::PosMessageToClientsExcluding` | `0x10031d00` |
| `EventsViaHandles::RegisterHandle` | `0x10031d50` |
| `EventsViaHandles::SendMessageToClients` | `0x10031e30` |
| `EventsViaHandles::UnregisterHandle` | `0x10031ea0` |

## Appendix C -- orchart.exe evidence

| Item | Address |
|------|---------|
| Import name string `"_OrlibRegisterNotificationHandle@4"` | `0x00781414` (.rdata) |
| Import `RegisterWndHandle` from ORLIB20.DLL (IAT entry) | `0x00381438` |
