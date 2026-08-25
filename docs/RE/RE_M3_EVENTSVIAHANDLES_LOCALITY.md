# RE_M3 — `EventsViaHandles` Notification HWND Set: Process Locality

> **Status:** Research-only findings note for Workstream M, slice M.3.
> **Scope:** Determine whether `orlib20.dll`'s `EventsViaHandles` notification HWND set
> is **process-local** (per-process DLL data) or **cross-process** (shared memory section).
> This determines whether the CLI's `0x40A` notification (sent from the CLI process's
> `orlib20.dll` instance) can reach a registered receiver window in a DIFFERENT process
> (e.g., TradeStation Desktop / `ORPlat.exe`).
> **Method:** Ghidra static analysis of `orlib20.dll` and `tslib.dll` (PE section headers,
> PE optional header, decompilation, disassembly, memory reads). All addresses are
> Ghidra program addresses (orlib20 loaded at base `0x10000000`).

---

## 0. Executive Answer

**CROSS-PROCESS.**

The `EventsViaHandles` notification HWND set is stored in a **PE shared memory section**
(`SHARE_WO`, marked `IMAGE_SCN_MEM_SHARED`). The CLI's `0x40A` posts **can** reach a
registered receiver window in a different process (e.g., TradeStation Desktop), because:

1. The HWND array lives in `SHARE_WO` (shared across all processes that load `orlib20.dll`).
2. Each process's `EventsViaHandles` object (in process-local `.data`) holds a pointer
   statically initialized to the **same shared array address**.
3. `orlib20.dll` has **no ASLR** (`DllCharacteristics = 0x0100`, no `DYNAMIC_BASE`), so it
   loads at `0x10000000` in every process — the shared section is at the same VA everywhere.
4. Registration uses `LOCK CMPXCHG` (hardware-atomic), the correct primitive for
   cross-process shared-memory concurrency — not a process-local critical section.
5. `PostMessageW` delivers to any HWND system-wide (HWNDs are global within a window
   session/desktop), so a post from the CLI process to Desktop's registered HWND succeeds.

**Implication for plan L218 (Desktop observation requirement):** The requirement is
**NOT moot**. The CLI's `0x40A` CAN reach Desktop's registered windows. Desktop
observation remains a valid verification target.

---

## 1. PE Section Analysis (orlib20.dll)

### 1.1 Section table (Ghidra `list_segments` + raw PE section header reads)

| Section    | VA range (base+RVA)              | Size       | Characteristics | Shared? |
|------------|----------------------------------|------------|------------------|---------|
| `.text`    | `0x10001000`–`0x101f83ff`        | 0x1f72fc   | `0x60000020`     | No (EXECUTE\|READ\|CODE) |
| `.rdata`   | `0x101f9000`–`0x1022d3ff`        | 0x3436a    | `0x40000040`     | No (READ) |
| **`.data`**| `0x1022e000`–`0x10240987`        | 0x12988    | **`0xC0004000`** | **No** (WRITE\|READ, no SHARED) |
| `.gfids`   | `0x10241000`–`0x102411ff`        | 0x200      | `0x40000040`     | No |
| `.tls`     | `0x10242000`–`0x102421ff`        | 0x200      | `0xC0000040`     | No |
| **`SHARE_WO`** | `0x10243000`–`0x104889a7`  | 0x2459a8   | **`0xD0004000`** | **YES** (WRITE\|READ\|**SHARED**) |
| **`SHARE_MM`** | `0x10489000`–`0x10489007`  | 0x8        | **`0xD0004000`** | **YES** (WRITE\|READ\|**SHARED**) |
| `.rsrc`    | `0x1048a000`–`0x104a5bff`        | 0x1bb00    | `0x40000040`     | No |
| `.reloc`   | `0x104a6000`–`0x104badff`        | 0x14e00    | `0x42000040`     | No |

**Evidence (raw PE section header reads via Ghidra `read_memory`):**

- `.data` header at `0x10000280`:
  `Name=".data" VA=0x0022e000 VSize=0x00012988 Chars=[0x00,0x40,0x00,0xC0]=0xC0004000`
  → `0xC0004000` = `IMAGE_SCN_MEM_WRITE(0x80000000)` | `IMAGE_SCN_MEM_READ(0x40000000)`
  | `0x00004000`. **`IMAGE_SCN_MEM_SHARED(0x10000000)` is NOT set.** Process-local.

- `SHARE_WO` header at `0x100002F8`:
  `Name="SHARE_WO" VA=0x00243000 VSize=0x002459a8 RawSize=0x00245a00 Chars=[0x00,0x40,0x00,0xD0]=0xD0004000`
  → `0xD0004000` = `WRITE(0x80000000)` | `READ(0x40000000)` | **`SHARED(0x10000000)`**
  | `0x00004000`. **`IMAGE_SCN_MEM_SHARED` IS set.** Cross-process shared memory.

- `SHARE_MM` header at `0x10000320`:
  `Name="SHARE_MM" VA=0x00489000 VSize=0x00000008 Chars=[0x00,0x40,0x00,0xD0]=0xD0004000`
  → Same flags as `SHARE_WO`: **SHARED**. (8-byte shared flag/counter; not directly
  relevant to the HWND set but confirms orlib20 uses shared sections by design.)

- `.text` header at `0x10000230` (calibration):
  `Chars=[0x20,0x00,0x00,0x60]=0x60000020` = `EXECUTE|READ|CODE`. ✓ Confirms the
  section-header parsing method is correct.

**Key observation:** The only difference between `.data` (`0xC0004000`) and `SHARE_WO`
(`0xD0004000`) is the `IMAGE_SCN_MEM_SHARED` bit (`0x10000000`). `.data` is private per
process; `SHARE_WO` is mapped as shared memory (same physical pages across all processes
that load `orlib20.dll`).

### 1.2 DLL ASLR / relocation characteristics

- **COFF `Characteristics`** = `0x2102` = `IMAGE_FILE_32BIT_MACHINE(0x0100)` |
  `IMAGE_FILE_DEBUG_STRIPPED(0x2000)` | `IMAGE_FILE_EXECUTABLE_IMAGE(0x0002)`.
  `IMAGE_FILE_RELOCS_STRIPPED(0x0001)` is **not** set (relocations are present).
- **Optional header `DllCharacteristics`** = `0x0100` =
  `IMAGE_DLLCHARACTERISTICS_NX_COMPAT`. `IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE(0x0040)`
  (ASLR) is **NOT set**.
- **`ImageBase`** = `0x10000000` (confirmed via optional header parse; consistent with
  Ghidra's load base and `SizeOfImage=0x004BB000` matching the VA span
  `0x10000000`–`0x104BADFF`).

**Implication:** Without ASLR, Windows loads `orlib20.dll` at its preferred base
`0x10000000` in every process (the default 32-bit DLL base, typically free). All
processes therefore map `SHARE_WO` at the **same virtual address** `0x10243000`–
`0x104889a7`, backed by the **same physical shared pages**. Even if ASLR were enabled,
Windows DLL ASLR randomizes once per boot per DLL (all processes get the same base), and
the loader fixes up `.data` pointers via relocations to point to each process's mapping
of the shared section — so cross-process sharing would still work. The absence of ASLR
makes the sharing mechanism straightforward and unambiguous.

---

## 2. `EventsViaHandles` Object and HWND Array Locations

### 2.1 The singleton object (process-local `.data`)

The `EventsViaHandles` singleton object is at **`orlib20!0x10231050`** (`PTR_DAT_10231050`
in Ghidra). This address falls in `.data` (`0x1022e000`–`0x10240987`):
`0x00231050` is in `[0x0022e000, 0x00240988]`. ✓ **Process-local.**

Memory read at `0x10231050` (16 bytes):
```
38 88 48 10  20 00 00 00  50 f3 1f 10  00 00 00 00
```
- `+0`: `0x10488838` — HWND array pointer (points into `SHARE_WO`)
- `+4`: `0x00000020` (32) — array slot count (fixed capacity)
- `+8`: `0x101ff350` — adjacent data (next object; not part of EventsViaHandles)

The `EventsViaHandles` class layout (from `tslib::EventsViaHandles::EventsViaHandles`
constructor decompilation at `tslib:0x10031ba0`):
```c
EventsViaHandles(HWND__ **param_1, ulong param_2) {
    *(HWND__ ***)this = param_1;   // +0: array pointer
    *(ulong *)(this + 4) = param_2; // +4: slot count
}
```

### 2.2 The HWND array (shared `SHARE_WO`)

The HWND array is at **`0x10488838`**. This address falls in `SHARE_WO`
(`0x10243000`–`0x104889a7`): `0x00488838` is in `[0x00243000, 0x004889a8]`. ✓
**Cross-process shared.**

The array sits at the very end of `SHARE_WO` (only `0x1748` bytes before the section's
virtual end at `0x104889a8`). It is `32 × 4 = 128` bytes of HWND slots.

Memory read at `0x10488838` (128 bytes, on-disk image):
```
all zeros (32 empty slots)
```
Slots are filled at runtime by `RegisterHandle` calls. The all-zero on-disk state
confirms no static HWND initialization — registration is purely runtime.

### 2.3 The sharing architecture

```
Process A (e.g., ORPlat.exe / Desktop)        Process B (e.g., tsdev_cli.exe / CLI)
┌─────────────────────────────────┐          ┌─────────────────────────────────┐
│ .data (PRIVATE per process)     │          │ .data (PRIVATE per process)     │
│  0x10231050: EventsViaHandles   │          │  0x10231050: EventsViaHandles   │
│    +0: 0x10488838 ──────────────┼──────────┼──→ 0x10488838                   │
│    +4: 32                       │          │    +4: 32                       │
└─────────────────────────────────┘          └─────────────────────────────────┘
                     │                                      │
                     │   SHARE_WO (SHARED — same physical   │
                     │   pages mapped at same VA 0x10243000)│
                     ▼                                      ▼
            ┌──────────────────────────────────────────────────┐
            │  0x10488838: HWND[32] array (128 bytes)          │
            │  [0] hwnd_desktop_chart  [1] hwnd_desktop_xxx    │
            │  [2] 0 (empty)  ...  [31] 0 (empty)              │
            └──────────────────────────────────────────────────┘
```

Each process has its **own** `.data` copy of the `EventsViaHandles` object (8 bytes:
pointer + count), but the pointer value (`0x10488838`) is identical in all processes
(statically initialized in `.data`, pointing into `SHARE_WO`). All processes' pointers
reference the **same physical shared HWND array**.

The count field (`+4 = 32`) is in process-local `.data`, but it is a **fixed capacity
constant** (never modified by `RegisterHandle`/`UnregisterHandle`/`PosMessageToClients` —
they only read it to compute the array end). Being the same constant `32` in all
processes, its process-locality is irrelevant.

---

## 3. Registration and Send Paths (Decompilation + Disassembly)

### 3.1 `_OrlibRegisterNotificationHandle@4` (orlib20 ordinal 595, `0x1011c130`)

Decompilation (Ghidra):
```c
void _OrlibRegisterNotificationHandle_4(HWND__ *param_1) {
    tslib::EventsViaHandles::RegisterHandle(
        (EventsViaHandles *)&PTR_DAT_10231050, param_1);
}
```
Thin wrapper: passes the singleton (`0x10231050`) as `this` and the caller's HWND as
the handle to register.

### 3.2 `tslib::EventsViaHandles::RegisterHandle` (`tslib:0x10031d50`)

Decompilation (Ghidra):
```c
bool RegisterHandle(EventsViaHandles *this, HWND__ *param_1) {
    if (param_1 == NULL) return false;
    // Phase 1: scan for existing match (idempotent re-register)
    int *p = *(int **)this;                      // = shared array @ 0x10488838
    int *end = p + *(int *)(this + 4);           // = array + 32
    for (; p != end; p++)
        if ((HWND__ *)*p == param_1) return true;
    // Phase 2: scan for empty slot, atomically claim it
    int retry = 1;
    do {
        p = *(int **)this;
        end = p + *(int *)(this + 4);
        for (; p != end; p++) {
            if (*p == 0) {
                LOCK();                          // ← LOCK CMPXCHG (hardware atomic)
                int old = *p;
                if (old == 0) { *p = (int)param_1; old = 0; }
                UNLOCK();
                if (old == 0) return true;       // successfully stored
            }
        }
        // No empty slot: try FreeInvalideHandles once, else throw
        if ((retry == 2 && !FreeInvalideHandles(this)) || ++retry > 2)
            AfxThrowInvalidArgException();       // array full (32 slots exhausted)
    } while (true);
}
```

**Disassembly of the atomic store** (`tslib:0x10031d94`–`0x10031da3`):
```asm
10031d94  CMP     dword ptr [ECX], 0x0       ; is slot empty?
10031d97  JNZ     0x10031da5                 ; no → next slot
10031d99  MOV     EDX, EBX                   ; EDX = new HWND
10031d9b  XOR     EAX, EAX                   ; EAX = 0 (expected current value)
10031d9d  CMPXCHG.LOCK dword ptr [ECX], EDX  ; F0 0F B1 11 — LOCK CMPXCHG
10031da1  TEST    EAX, EAX                   ; did CMPXCHG succeed (old==0)?
10031da3  JZ      0x10031dd3                 ; yes → return true
```

The `LOCK CMPXCHG` (`F0 0F B1 11`) is a **hardware-bus-locked compare-and-exchange**.
This is the correct synchronization primitive for cross-process shared memory:
- It atomically compares `[ECX]` with `EAX` (0); if equal, stores `EDX` (the HWND) into
  `[ECX]`.
- The `LOCK` prefix (`F0`) asserts a hardware bus lock, providing atomicity across CPU
  cores **and across processes** sharing the same physical page.
- A process-local `CRITICAL_SECTION` would NOT protect against concurrent access from
  other processes. The use of `LOCK CMPXCHG` (not a critical section) confirms the
  design intent for cross-process shared-memory concurrency.

### 3.3 `_OrlibUnregisterNotificationHandle@4` (orlib20 ordinal 596, `0x1011c140`)

Decompilation (Ghidra):
```c
void _OrlibUnregisterNotificationHandle_4(HWND__ *param_1) {
    tslib::EventsViaHandles::UnregisterHandle(
        (EventsViaHandles *)&PTR_DAT_10231050, param_1);
}
```
`UnregisterHandle` (`tslib:0x10031ea0`) uses the same `LOCK`-guarded atomic clear
pattern: scans the shared array, atomically zeroes the matching slot.

### 3.4 Sender `FUN_101526f0` (orlib20 ordinal 57, `0x101526f0`)

Decompilation (Ghidra):
```c
undefined4 FUN_101526f0(undefined2 category, undefined2 progId) {
    tslib::EventsViaHandles::PosMessageToClients(
        (EventsViaHandles *)&PTR_DAT_10231050,
        0x40a, 0, CONCAT22(category, progId));
    return 1;
}
```
Passes the singleton (`0x10231050`) as `this`. The `CONCAT22(category, progId)` packs
`(category << 16) | (progId & 0xFFFF)` into `lParam` (per RE_M3_0x40A_NOTIFICATION.md §1.1).

### 3.5 `tslib::EventsViaHandles::PosMessageToClients` (`tslib:0x10031cb0`)

Decompilation (Ghidra):
```c
void PosMessageToClients(EventsViaHandles *this, int msg, int wParam, int lParam) {
    undefined4 *p = *(undefined4 **)this;        // = shared array @ 0x10488838
    uint count = *(int *)(this + 4);            // = 32
    uint n = (count * 4 + 3) >> 2;              // = 32
    if (p + count < p) n = 0;                   // overflow guard (no-op for 32)
    uint i = 0;
    if (n != 0) {
        do {
            if ((HWND)*p != (HWND)0x0)
                PostMessageW((HWND)*p, msg, wParam, lParam);  // cross-process post
            i++;
            p++;
        } while (i != n);
    }
}
```

**This reads from the shared array** (`0x10488838` in `SHARE_WO`) and calls
`PostMessageW` for each non-zero HWND. `PostMessageW` delivers to any HWND
system-wide — the target window's owning thread/process is resolved by the OS, and the
message is posted to that thread's queue. A post from the CLI process to Desktop's
registered HWND is a normal cross-process `PostMessageW`.

---

## 4. Why the CLI's `0x40A` CAN Reach Desktop

Step-by-step data flow:

1. **Desktop registers:** When TradeStation Desktop (`ORPlat.exe`) or its child modules
   (e.g., `orchart.exe`, confirmed to import `_OrlibRegisterNotificationHandle@4` per
   RE_M3_0x40A_NOTIFICATION.md §1.4) start, they call
   `_OrlibRegisterNotificationHandle@4(hwnd)` with their window HWNDs. This writes the
   HWNDs into **shared array slots** at `0x10488838` in `SHARE_WO`.

2. **CLI sends:** When `tsdev_cli.exe` (a separate process) successfully
   inserts/modifies/deletes/verifies a library item, it calls orlib20 Ordinal_57
   (`FUN_101526f0`) → `PosMessageToClients`. This reads the **same shared array** at
   `0x10488838` and calls `PostMessageW(0x40A, 0, packed_lParam)` for every non-zero
   slot — including Desktop's HWNDs.

3. **Cross-process delivery:** `PostMessageW` resolves the HWND to the owning thread
   (in Desktop's process) and posts `0x40A` to that thread's message queue. Desktop's
   window procedure receives and handles `0x40A` (refreshing in-memory library state),
   exactly as it would if TSDev.exe had sent the notification.

**The CLI does not need to know Desktop's HWNDs.** It simply calls the same orlib20
export TSDev uses; the shared array automatically contains all registered receivers
from all processes.

---

## 5. Relationship to Existing RE_M3_0x40A_NOTIFICATION.md

The existing note (RE_M3_0x40A_NOTIFICATION.md) documents the `0x40A` message format,
send sites, delivery mechanism, and Frida confirmation, but does **not** address process
locality of the `EventsViaHandles` HWND set. Its §5 item 5 ("Complete receiver
inventory") notes the 19 Frida-observed HWNDs "belong to whatever TradeStation processes
were running at capture time" — hinting at cross-process but not confirming it.

This note fills that gap with definitive PE section analysis. The existing note's
claims are fully consistent with the cross-process finding:
- The singleton `PTR_DAT_10231050` (§1.4) is the `EventsViaHandles` object in `.data`
  (process-local), but its array pointer targets `SHARE_WO` (shared).
- The Frida trace's 19 receiver HWNDs (§1.5) likely include windows from multiple
  TradeStation processes (TSDev + Desktop + orchart), all registered in the shared array.
- The test-receiver design (§4) works cross-process: a test receiver in a separate
  process registering via `_OrlibRegisterNotificationHandle@4` will receive `0x40A`
  from any sender process, including the CLI.

---

## 6. Caveats and Limitations

1. **`SHARE_WO` is 2.38 MB; the HWND array is 128 bytes at its end.** The vast majority
   of `SHARE_WO` holds other shared orlib20 state (likely the warehouse cache, global
   lookup tables, etc.). The HWND array is a small data structure within a large shared
   section. This is consistent with orlib20 being a shared library layer with
   extensive cross-process state.

2. **Receiver inventory not exhaustive.** `ORPlat.exe` does not directly import
   `_OrlibRegisterNotificationHandle@4` (Ghidra import search found no match). Desktop
   likely registers via loaded DLLs (e.g., `orchart.exe` is a confirmed registrant;
   other DLLs like `ELRte.dll`, `ELMDCore.dll`, `ECL.dll` were checked and do not
   directly import it either). The registration may occur through a deeper wrapper or
   a different orlib20 export not yet identified. The cross-process sharing mechanism
   is confirmed regardless of which specific module registers — any process that loads
   orlib20 and calls the registration API writes to the shared array.

3. **`PostMessageW` cross-process requires same window session/desktop.** The CLI and
   Desktop must run in the same Windows session and desktop (the standard case for
   TradeStation automation). If they ran in different sessions (e.g., different
   Terminal Services sessions), `PostMessageW` would not deliver. This is not a concern
   for the TSDev Automation use case (single interactive session).

4. **`0x40A` is live-only, not a durable queue.** If Desktop is not running when the CLI
   sends `0x40A`, the notification is silently dropped (no registered HWND for Desktop).
   Restarting Desktop does not replay missed notifications. This is unchanged from the
   existing note's §6 item 6.

---

## 7. Evidence Index

| Claim | Source |
|-------|--------|
| `SHARE_WO` section has `IMAGE_SCN_MEM_SHARED` | PE section header at `0x100002F8`: Chars bytes `[0x00,0x40,0x00,0xD0]` = `0xD0004000` (WRITE\|READ\|**SHARED**); Ghidra `read_memory` |
| `.data` section does NOT have `IMAGE_SCN_MEM_SHARED` | PE section header at `0x10000280`: Chars bytes `[0x00,0x40,0x00,0xC0]` = `0xC0004000` (WRITE\|READ, no SHARED); Ghidra `read_memory` |
| `SHARE_MM` section has `IMAGE_SCN_MEM_SHARED` | PE section header at `0x10000320`: Chars `0xD0004000` (SHARED); Ghidra `read_memory` |
| `.text` section Chars = `0x60000020` (calibration) | PE section header at `0x10000230`: bytes `[0x20,0x00,0x00,0x60]`; Ghidra `read_memory` |
| `SHARE_WO` VA range = `0x10243000`–`0x104889a7` | Section header: VA=`0x00243000`, VSize=`0x002459a8`; Ghidra `list_segments` confirms `0x10243000`–`0x104889ff` |
| `.data` VA range = `0x1022e000`–`0x10240987` | Section header: VA=`0x0022e000`, VSize=`0x00012988`; Ghidra `list_segments` |
| EventsViaHandles object at `0x10231050` is in `.data` | `0x00231050` ∈ `[0x0022e000, 0x00240988]` ✓; Ghidra `read_memory` at `0x10231050` |
| Object layout: `+0=0x10488838` (array ptr), `+4=0x20` (count=32) | Ghidra `read_memory` at `0x10231050`: bytes `38 88 48 10 20 00 00 00` |
| HWND array at `0x10488838` is in `SHARE_WO` | `0x00488838` ∈ `[0x00243000, 0x004889a8]` ✓; Ghidra `read_memory` at `0x10488838` (128 bytes of zeros) |
| `EventsViaHandles` constructor sets `+0=array, +4=count` | Ghidra decompile `tslib:0x10031ba0` (`EventsViaHandles::EventsViaHandles`) |
| `RegisterHandle` uses `LOCK CMPXCHG` (hardware atomic) | Ghidra disasm `tslib:0x10031d9d`: `CMPXCHG.LOCK dword ptr [ECX], EDX` (bytes `F0 0F B1 11`) |
| `PosMessageToClients` reads shared array + `PostMessageW` per slot | Ghidra decompile `tslib:0x10031cb0` |
| `_OrlibRegisterNotificationHandle@4` = ordinal 595 @ `0x1011c130` | Ghidra decompile annotation `/* 0x11c130 595 */`; calls `RegisterHandle(&PTR_DAT_10231050, param_1)` |
| `_OrlibUnregisterNotificationHandle@4` = ordinal 596 @ `0x1011c140` | Ghidra decompile annotation `/* 0x11c140 596 */`; calls `UnregisterHandle` |
| Sender `FUN_101526f0` = ordinal 57, passes `&PTR_DAT_10231050` as `this` | Ghidra decompile `0x101526f0` |
| No ASLR (`DllCharacteristics` = `0x0100`, no `DYNAMIC_BASE`) | PE optional header `DllCharacteristics` at `0x10000196`: bytes `[0x00,0x01]` = `0x0100` = `NX_COMPAT` only |
| `ImageBase` = `0x10000000` | PE optional header `ImageBase`; `SizeOfImage=0x004BB000` matches VA span `0x10000000`–`0x104BADFF` |
| `orchart.exe` imports `_OrlibRegisterNotificationHandle@4` | RE_M3_0x40A_NOTIFICATION.md §1.4 (orchart IAT) |
| `PostMessageW` works cross-process | Win32 API: HWNDs are global within a window session/desktop; `PostMessageW` resolves target thread across process boundaries |
