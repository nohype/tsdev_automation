# TSDev Knowledge Base

Current proven knowledge only. Every statement cites evidence. No assumptions, conclusions, history, or unevidenced claims.
No decision record.

---

## 1. orlib20.dll basics

- Base address: `0x10000000` (Ghidra project `TSDevAnalysis`)
- Target version: v9.50.01.3344 (`signatures/v9.50.01.3344.json`)
- 32-bit DLL; CLI and all callers must be 32-bit (`AGENTS.md`)

---

## 2. Warehouse memory layout

The warehouse is a singleton obtained via `GetWarehouse(NULL)`. All offsets are relative to the warehouse base pointer.

| Offset | Size | Field | Evidence |
|--------|------|-------|----------|
| `+0x04` | 4 | Command ID (written by dispatchers, read by worker) | Ghidra `FUN_101cf5b0` @ `0x101cf5b0`: `*(param_1+4) = param_2` |
| `+0x12` | 2 | whType (category, low 8 bits of ComponentType) | Ghidra `FUN_101d0950` @ `0x101d0950`: `MOV WORD PTR [ESI+0x12], param_3` |
| `+0x16` | 4 | ProgId / name argument (for cmd 0x0D) | Ghidra worker cmd 0x0D @ `0x101d2800`: `PUSH [EDI+0x16]` |
| `+0x1E` | 16 | Load struct / VARIANT-style slot (for cmd 0x0F input and cmd 0x3B output) | Ghidra worker cmd 0x0F @ `0x101d2891`: `MOVUPS XMM0, [EDI+0x1E]`; cmd 0x3B @ `0x101d3156`: `LEA EAX, [EDI+0x1E]` |
| `+0x26` | 4 | SafeArray result pointer (VARIANT value field at `+0x1E+8`) | Ghidra `FUN_101c51b0` @ `0x101c51b0`: reads `*(warehouse+0x26)` as `SAFEARRAY*` |
| `+0x4E` | 4 | HRESULT / status (0 = success) | Ghidra worker: `MOV [EDI+0x4E], EAX` after each vtable call |
| `+0x5E` | 4 | IProgram pointer | Ghidra worker cmd 0x0D @ `0x101d283e`: `MOV [EDI+0x5E], EAX`; cmd 0x0F @ `0x101d28d9`: `MOV [EDI+0x5E], EAX` |
| `+0xAE` | 4 | Programs-collection pointer (COM server interface) | Ghidra worker cmd 0x0D @ `0x101d2800`: `MOV ECX, [EDI+0xAE]`; cmd 0x0F @ `0x101d2891`: same |
| `+0x6A` | 4 | Worker completion event (HANDLE) | Ghidra `FUN_101cf5b0`: `WaitForSingleObject([param_1+0x6A], INFINITE)` |
| `+0x6E` | 4 | Worker wake event (HANDLE) | Ghidra `FUN_101cf5b0`: `SetEvent([param_1+0x6E])` |
| `+0x72` | 4 | Worker mutex (HANDLE) | Ghidra `FUN_101cf5b0`: `WaitForSingleObject([param_1+0x72], INFINITE)` / `ReleaseMutex` |
| `+0x76` | 4 | Worker thread ID | Ghidra `FUN_101cf5b0`: `GetCurrentThreadId() != *(param_1+0x76)` |

---

## 3. Worker thread command dispatch

### Generic dispatcher: `FUN_101cf5b0` (RVA `0x1cf5b0`, abs `0x101cf5b0`)

`__thiscall`: ECX = warehouse, stack arg = cmd number.

Sequence (Ghidra decompilation):
1. `WaitForSingleObject([warehouse+0x72], INFINITE)` — acquire mutex
2. Thread-affinity check: `GetCurrentThreadId() != [warehouse+0x76]` and cmd != 0x1E → `swi(3)` (crash)
3. `[warehouse+0x04] = cmd`
4. `SetEvent([warehouse+0x6E])` — wake worker
5. `WaitForSingleObject([warehouse+0x6A], INFINITE)` — wait for completion
6. `ReleaseMutex([warehouse+0x72])`

### Worker thread: `FUN_101d2150` (RVA `0x1d2150`, abs `0x101d2150`)

Reads `[warehouse+0x04]`, decrements by 1, bounds-checks against `0x79`, jumps through table at `0x101d3680` (`JMP dword ptr [EAX*4 + 0x101d3680]`).

---

## 4. Commands

### cmd 0x0D — FindProgram

- Worker handler at `0x101d2800` (Ghidra disassembly)
- Calls `programs_collection->vtable[0x20]` (FindProgram) with `(whType=[+0x12], name=[+0x16], &out_IProgram)`
- On success: `[warehouse+0x5E] = IProgram` (record-reference)
- On failure: `[warehouse+0x5E] = 0` (at `0x101d285d`)
- The dispatcher `FUN_101cfce0` (RVA `0x1cfce0`) reads `[+0x5E]` and immediately clears it (borrow pattern): `uVar3 = *(warehouse+0x5E); *(warehouse+0x5E) = 0; return uVar3;`
- The IProgram from cmd 0x0D is a record-reference — its caller/dependency tables are not populated
- **Cross-process recalc trigger:** Dispatching cmd 0x0D with `(category, progId)` fires WHServer.exe COM events (`_IWHouseEvents`) that reach orchart.exe and trigger dependent recalculation. Confirmed at runtime (KB §13).

### cmd 0x0F — LoadProgram

- Worker handler at `0x101d2891` (Ghidra disassembly)
- Calls `programs_collection->vtable[0xA8]` (LoadProgram) with `(16-byte struct from [+0x1E], whType=[+0x12], &out_IProgram)`
- On success: `[warehouse+0x5E] = IProgram` (fully-loaded)
- On failure: `[warehouse+0x5E] = 0` (at `0x101d285d`)
- The IProgram from cmd 0x0F is fully-loaded — its caller/dependency tables are populated by the load
- **Must be dispatched via specialized `FUN_101d0950`, not generic `FUN_101cf5b0`** — the generic dispatcher does not set up the 16-byte load struct at `+0x1E` and returns `E_INVALIDARG` (`0x80070057`). Proven by dynamic test: generic dispatch of 0x0F returned status `0x80070057`, `warehouse+0x5E = NULL`, followed by crash.

### cmd 0x3B — IProgram::Callers

- Worker handler at `0x101d3156` (Ghidra disassembly)
- `MOV ECX, [EDI+0x5E]` — reads IProgram from `warehouse+0x5E` directly (**no NULL check**)
- `LEA EAX, [EDI+0x1E]` — out-param is `&warehouse+0x1E`
- `MOV EDX, [ECX]; CALL [EDX+0x38]` — calls `IProgram->vtable[0x38]` (Callers)
- `MOV [EDI+0x4E], EAX` — HRESULT to `warehouse+0x4E`
- SafeArray pointer lands at `warehouse+0x26` (VARIANT value field at `+0x1E+8`)
- **NULL `+0x5E` causes access violation** at `MOV EDX, [ECX]` (dereferences NULL). Proven by dynamic test: crash + `TSCrashReport.exe` dialog.
- **Returns empty SafeArray (count=0) regardless of dispatch path** — see KB §9.

### cmd 0x4E — Cache refresh

- Sets `[warehouse+0x12] = 0xC` before dispatch
- Does not write `warehouse+0x5E`

### cmd 0x4F — Release

- Dispatcher: `FUN_101c69b0` (RVA `0x1c69b0`, abs `0x101c69b0`)
- Clears `warehouse+0x5E` to NULL at instruction `0x101c6a06`: `MOV dword ptr [EAX+0x5E], 0x0`
- Called by program-writer destructor chain: `FUN_101c0fd0 → FUN_101c0ca0 → FUN_101c69b0`

---

## 5. Specialized cmd 0x0F dispatcher: `FUN_101d0950`

RVA `0x1d0950`, abs `0x101d0950`. `__thiscall`: ECX = warehouse, 3 stack args (typeArray, progId, outParam).

### What it does (Ghidra decompilation + disassembly)

1. Checks `[warehouse+0xAE] != 0` (COM server initialized); bail if NULL
2. Sets `[warehouse+0x1E] = 0x2000` (load flags)
3. Calls `FUN_101d1010(typeArray)` — creates SafeArray from type array
4. Stores SafeArray at `[warehouse+0x26]`
5. Sets `[warehouse+0x12] = progId` (the 3rd param, despite the name)
6. Acquires mutex (`WaitForSingleObject([+0x72])`), checks thread affinity
7. Sets `[warehouse+0x04] = 0x0F`, signals worker (`SetEvent([+0x6E])`), waits (`WaitForSingleObject([+0x6A])`)
8. Releases mutex
9. `SafeArrayDestroy([warehouse+0x26])` — destroys the input SafeArray
10. If `[warehouse+0x4E] == 0` (success): calls `FUN_101c4ab0(prog_writer, warehouse, [warehouse+0x5E])` to copy IProgram into prog_writer, sets `prog_writer+0x26D = 1`, returns 1
11. Returns 0 on failure

### +0x5E survival

**`FUN_101d0950` does NOT clear `warehouse+0x5E`.** It only reads `[warehouse+0x5E]` once at `0x101d09c7` (`PUSH [ESI+0x5E]`) to pass it to `FUN_101c4ab0`. There is no `MOV [ESI+0x5E], 0` anywhere in the function. The cleanup that clears `+0x5E` is the caller's responsibility.

### `FUN_101c4ab0` (RVA `0x1c4ab0`)

`__thiscall`: ECX = prog_writer, stack = (warehouse, IProgram).
- `prog_writer+0x0C = warehouse` (back-pointer)
- `prog_writer+0x10 = IProgram` (copy from `warehouse+0x5E`)
- Does NOT clear `warehouse+0x5E`

---

## 6. orlib20's own cmd 0x3B dispatcher: `FUN_101c51b0`

RVA `0x1c51b0`, abs `0x101c51b0`. `__fastcall`: ECX = context object (not warehouse).

### What it does (Ghidra decompilation)

1. Re-entrance guard: checks `[param_1+0x1C9]`
2. Acquires `CSingleLock` on `[param_1+0x218]`
3. **Cache check**: if `[param_1+0x10] == 0` (cache empty):
   - Calls `FUN_101cfce0` (cmd 0x0D / FindProgram) with `(whType=[param_1+0x15], name=[param_1+0x18])`
   - Stores result IProgram into `[param_1+0x10]` (cache)
   - If result == 0: `swi(3)` (crash — item not found)
4. **Writes cached IProgram to warehouse**: `*(warehouse+0x5E) = *(param_1+0x10)` where `warehouse = *(param_1+0x0C)`
5. Dispatches cmd 0x3B via `FUN_101cf5b0(0x3B)`
6. Checks `[warehouse+0x4E] == 0` (success)
7. If success and `FUN_101c1ed0([warehouse+0x26]) != 0` (element count != 0):
   - `SafeArrayAccessData([warehouse+0x26], &buf)`
   - `FUN_101e1560(warehouse, buf, elemCount)` — parse caller records
   - `SafeArrayUnaccessData` + `SafeArrayDestroy`

**Key fact**: orlib20's own 0x3B path uses a **FindProgram** (cmd 0x0D) IProgram, not a LoadProgram IProgram. The cache at `param_1+0x10` is populated by FindProgram and reused.

### Context object layout (inferred from decompilation)

| Offset | Field |
|--------|-------|
| `+0x0C` | warehouse pointer |
| `+0x10` | cached IProgram (populated by cmd 0x0D) |
| `+0x15` | whType (short) |
| `+0x18` | program name |
| `+0x1C9` | re-entrance guard |
| `+0x218` | CSyncObject* for CSingleLock |

---

## 7. `_VerifyModule_New@20`

RVA `0x169630`, abs `0x10169630`. Ordinal 700. `__stdcall`: 5 stack args (20 bytes).

### What it does (Ghidra decompilation)

1. Gets warehouse via `FUN_101a9650(0)`
2. Setup: `FUN_101d43a0`, `FUN_101cff60`, `FUN_10168e70`, `FUN_101a07f0`, `FUN_101087d0` (load source text)
3. Constructs `CELBasicCompiler`
4. Calls `CELBasicCompiler::CompileText` with `param_6 = 0` (LoadProgram path)
5. `GlobalFree(hMem)`
6. `FUN_101a9a90()` — dispatches cmd 0x13
7. `CELBasicCompiler::~CELBasicCompiler`
8. `FUN_101698f5()` — tree-walk cleanup
9. Returns

### +0x5E state after return: NULL

`warehouse+0x5E` is cleared **inside** `CompileText` before `_VerifyModule_New` returns. The clearing happens via the program-writer destructor chain:

```
CompileText @ 0x1001dd80
  └─ FUN_101d0950(...)          ← sets +0x5E (worker loads IProgram)
  └─ FUN_101c0fd0(&prog_writer) ← caller cleanup
       └─ FUN_101c0ca0(&prog_writer)
            └─ if (prog_writer+0x10 != 0)
                 └─ FUN_101c69b0(&prog_writer)  ← cmd 0x4F Release
                      ├─ warehouse+0x5E = IProgram (temp re-arm)
                      ├─ dispatch cmd 0x4F
                      └─ warehouse+0x5E = 0  ★ CLEARED at 0x101c6a06
```

There is no window after `_VerifyModule_New` returns where `+0x5E` is valid.

---

## 8. SafeArray format (cmd 0x3B result)

Proven by dynamic test + Ghidra `FUN_101c1ed0`. Dynamic-test SafeArray dump (zero-caller case, captured via Frida on the UI's `FUN_101c51b0` path): `cDims=1, fFeatures=0x80 (FADF_HAVEVARTYPE), cbElements=1, cLocks=0, pvData=<ptr>, cElements=4, lLbound=0, totalBytes=4`; first 4 bytes: `00 00 00 00`; count prefix (little-endian int32): `0`.

- Type: `VT_UI1` (unsigned 1-byte elements)
- Dimensions: 1
- `cbElements = 1`
- `cElements = 4` (for zero-caller case)
- `lLbound = 0`
- Total bytes = `cElements * cbElements` = 4
- First 4 bytes = count prefix (little-endian int32)
- Entries start at byte offset 4
- Entry format: `+0x00: 2 bytes type/category, +0x02: 4 bytes ProgId, +0x06: 2 bytes name length, +0x08: name bytes, +0x08+nameLen: 0x16 (22) bytes metadata`
- Entry stride = `8 + nameLength + 0x16`

When count prefix = 0 and total bytes = 4: zero callers.

---

## 9. cmd 0x3B returns empty SafeArray regardless of dispatch path (dependents workstream — PAUSED)

The `dependents` workstream is paused (beyond current acceptance target). The following proven fact is recorded for future reference if that workstream resumes:

**cmd 0x3B on a function's IProgram returns an empty SafeArray (count=0, 4 bytes all zeros) regardless of dispatch path** — both the CLI's direct `FUN_101cf5b0(0x3B)` path and the UI's `FUN_101c51b0` path produce the same empty result, even for persisted items with a genuine compiler-resolved caller relationship. The cause is downstream of persistence: the dependency/caller table that 0x3B reads is not populated by the insert/compile path. The next investigation (if resumed) should focus on whether the WHServer.exe COM server's dependency database is populated at all, e.g. query `GetStaticDependencyList`/`GetDynamicDependencyList` via orlib20 (orlib20.dll exports these) or inspect the warehouse dependency tables directly.

Evidence:
- UI path (`FUN_101c51b0`), Frida trace on function ProgId=80 (IProgram=0x179e160c) called by indicator ProgId=2990: cmd 0x3B dispatched with `wh+5E=0x179e160c`, SafeArray result `cElements=4, cbElements=1`, first 4 bytes `00 00 00 00`, count prefix=0, 0 entries parsed. Identical result on a second trace with ProgId=82 (IProgram=0x12b52a64).
- CLI path (`FUN_101cf5b0` direct), function ProgId=517: cmd 0x0F succeeded, cmd 0x3B succeeded (status=0), SafeArray `elemCount=4, cbElements=1`, 4 bytes all zeros, count prefix=0.
- Persistence-ruled-out: CLI-inserted function `E2eFnDep_113051` (ProgId=1591, `exists=1`) and indicator `E2eIndDep_113051` (ProgId=2995, `exists=1`, compiled successfully while referencing the function — genuine compiler-resolved caller relationship). `tsdev_cli dependents function E2eFnDep_113051` returned `dependents_count=0`; internal trace shows cmd 0x3B returned a 4-byte all-zero SafeArray. Both items cleaned up (`exists=0`).

TSDev.exe has NO "Find Dependents" UI feature for regular functions/indicators. Evidence:
- `win32gui.GetMenu` on the main window returns `hmenu=0x0` (TSDev uses a BCGPSoft owner-drawn `BCGPToolBar` menu bar, not a standard Win32 HMENU); pywinauto `menu()` returns None; UIA backend found only the system MenuBar; Alt+<key> walks produced no `#32768` popup windows.
- `strings` on TSDev.exe (narrow + wide) found NO "Find Dependents", "Find References", "Where Used", or "Used By" menu text. The strings "Dependencies" and "Callers" exist only in a sequence of TradingApp project resource tree node names (`TradingApp, UserFunction, SystemFunction, Icon, Form, Dependencies, Strings, Callers, Image`), not a function/indicator feature.
- Opening a function (built-in `Average` and user `E2eFn_202608291052`) in the editor and dumping the Outline pane `SysTreeView32` showed only a `Document` node (with `using`, `inputs`, `Price`, `Length` children). No `Callers` or `Dependencies` node.

---

## 10. TS Desktop cross-process notification mechanism

### Shared `SHARE_WO` HWND array

The `EventsViaHandles` HWND array buffer is in the shared `SHARE_WO` PE section (`IMAGE_SCN_MEM_SHARED`, char `0xD0000040`) at `0x10488838` (capacity 32). Any process loading the same `orlib20.dll` sees the same pre-populated array. Confirmed at runtime: the CLI observed 19 pre-populated non-zero slots it did not register, and 16 orchart `DispatchMessageW` arrivals were confirmed for `PostMessageW(0x40A, 0, 0x10054)` (cat=1, progId=84). SSOT: `docs/RE/RE_OrlibRegisterNotificationHandle.md` §2.2.

### NotifyLibChanged (orlib20 ordinal 57)

orlib20 `FUN_101526f0` (ordinal 57, `_NotifyLibChanged@8`) packs `lParam = (category << 16) | progId` and calls `tslib::EventsViaHandles::PosMessageToClients(&PTR_DAT_10231050, 0x40A, 0, lParam)`, which iterates the HWND array and calls `PostMessageW(hwnd, 0x40A, 0, lParam)` per registered HWND. Runtime-confirmed: 19 `PostMessageW(0x40A)` messages posted inside ORD57 to 16 `orchart.exe` HWNDs, 2 `TSELAppHost`, 1 `TSScanner` (zero `TSDev.exe`). HWND ownership verified via `GetWindowThreadProcessId` + `GetClassName` on all 19 HWNDs: 16 orchart HWNDs (class `Afx:00400000:8:00010003:...`), 2 TSELAppHost HWNDs (class `TSMSGWNDCLASSu`), 1 TSScanner HWND (class `TSMSGWNDCLASSu`). `lParam=0x10054 = (category=1 << 16 | progId=84)`.

### Notification registration exports

- `_OrlibRegisterNotificationHandle@4` (ordinal 595, `FUN_1011c130` @ `0x1011c130`) calls `EventsViaHandles::RegisterHandle(&PTR_DAT_10231050, hwnd)`.
- `_OrlibUnregisterNotificationHandle@4` (ordinal 596, `FUN_1011c140` @ `0x1011c140`) unregisters.

### Delete-side additional messages

orlib20's delete path (`FUN_10159ae0` @ `0x10159ae0`) sends `SendMessageToClients(0x40C, 1, ...)` (delete start), `0x40C, 2` (in progress), and `0x40C, 3` (delete end) around the delete orchestrator, plus `PosMessageToClients(0x40A, 0, ...)` (library changed) when `category != 4`.

### Shared warehouse + WHServer.exe COM Connection Points

orlib20's warehouse data lives in the `SHARE_WO` section (`0x10243000-0x104889ff`, 2.38 MB, shared across all processes loading orlib20). orlib20's `_CreateEntryEX@24` (`FUN_10159610` @ `0x10159610`) makes a vtable call through `warehouse+0xAE` (the WHServer.exe COM server interface). WHServer.exe implements `IConnectionPointContainerImpl<CWHouse>` with `_IWHouseEvents` (RTTI strings at `0x0043ea4c`, `0x0043ea80` in WHServer.exe) and `IConnectionPointContainerImpl<CProgram>` with `_IProgramEvents`. TS Desktop subscribes to `_IWHouseEvents` via `IConnectionPoint::Advise`.

---

## 11. Read-only function classification (CONFIRMED ROOT CAUSE)

The warehouse category (low 8 bits of `whType`) controls whether TSDev.exe treats a function as editable or read-only. Category `1` is the editable user function; category `4` is the read-only built-in function.

### Type and name mapping (category 1 vs category 4)

| Category | orlib20 function | Address | Output | Evidence |
|----------|------------------|---------|--------|----------|
| 1 | `GetFileTypeName1` | `0x10157dc0` | `"Function"` (editable) | orlib20 decompilation / disassembly |
| 4 | `GetFileTypeName1` | `0x10157dc0` | `"Function (Read Only)"` | orlib20 decompilation / disassembly |
| 1 | `FUN_101583c0` | `0x101583c0` | `"Function"` | orlib20 decompilation / disassembly |
| 4 | `FUN_101583c0` | `0x101583c0` | `"FunctionReadOnly"` | orlib20 decompilation / disassembly |

### `READONLY` property construction

| Function | orlib20 address | Role | Evidence |
|----------|-----------------|------|----------|
| `FUN_10168b20` | `0x10168b20` | Builds property data containing `"READONLY" = "TRUE"` for category-4 functions | orlib20 decompilation / disassembly |
| `FUN_10170a30` | `0x10170a30` | Builds property data containing `"READONLY" = "TRUE"` for category-4 functions | orlib20 decompilation / disassembly |
| `FUN_101a1bd0` | `0x101a1bd0` | Builds property data containing `"READONLY" = "TRUE"` for category-4 functions | orlib20 decompilation / disassembly |

### `SaveModule_New` flag mapping

| Fact | Evidence |
|------|----------|
| orlib20 `_SaveModule_New@20` is `FUN_10159770` at `0x10159770` | orlib20 decompilation / disassembly |
| Maps the `saveFlags` word to master-record flag bits | Ghidra decompilation of `FUN_10159770` |
| Low 3 bits of `saveFlags` are the warehouse category: `saveFlags = (category & 0x7) \| 0x60` | Ghidra decompilation of `FUN_10159770` |
| The read-only classification does **not** come from a separate read-only bit — orlib20 derives `READONLY="TRUE"` from the category value itself (`4` = read-only) | orlib20 decompilation / disassembly |
| Internal default password is `!p$Kn@aSus` (10 bytes in a 64-byte zeroed buffer) | orlib20 decompilation / disassembly |

### Master-record offsets (fresh function, category 1)

| Offset | Size | Value | Interpretation |
|--------|------|-------|----------------|
| `+0x7B` | USHORT | `0x0401` | Low byte = warehouse category 1; bit 10 (`0x400`) set = internal-encryption flag |
| `+0x109` | byte | `0x18` | Baseline byte for a fresh, production-path function |

### TSDev.exe category assignment

| Fact | Address / Source | Evidence |
|------|------------------|----------|
| TSDev.exe creates user functions with category `1` | `CELFunctionDoc` ctor @ `0x00457eb0` | TSDev.exe Ghidra decompilation |
| TSDev.exe creates built-in functions with category `4` | `CELBIFunctionDoc` ctor @ `0x004140c0` | TSDev.exe Ghidra decompilation |
| TSDev.exe's save path passes `param_1[0x7a]` (document category) to `_SaveModule_New@20` | `FUN_00452290` @ `0x00452290` | TSDev.exe Ghidra decompilation |
| orlib20 places categories `1` and `4` in separate warehouse search trees | `FUN_101d43a0` @ `0x101d43a0` | orlib20 decompilation / disassembly |

---

## 12. TSDev UI save path vs CLI modify path — ordinal call diff

Static + runtime analysis of the TSDev.exe function-edit-save path. All evidence from Ghidra decompilation of `TSDev.exe` and `orlib20.dll` plus runtime Frida traces.

### TSDev UI save path (`FUN_00451800` @ `0x00451800`) — ordinals called

| Step | Call | orlib20 address / ordinal | CLI equivalent |
|------|------|--------------------------|----------------|
| 5 | **Ordinal_25** `(category, progId, name)` | `FUN_10151b10` @ `0x10151b10` | **MISSING** |
| 8 | **Ordinal_56** `()` | `FUN_10158b70` @ `0x10158b70` | **MISSING** |
| 10 | `FUN_00452290` → `_SaveModule_New@20` | `FUN_10159770` @ `0x10159770` | `saveSource()` ✓ |
| 12 | **Ordinal_66** `(compiler alloc)` | `FUN_10165f70` @ `0x10165f70` | ✓ |
| 14 | `FUN_004D9480` → `StartCompilingProcess` / `FUN_004D91E0` / `EndCompilingProcess` | internal TSDev | `verifyModule()` partial ✓ |
| 14a | **Ordinal_10** `(0)` — inside `FUN_004D91E0` category-gated block | `FUN_101a9650` @ `0x101a9650` | **MISSING** |
| 14b | `FUN_004D7140` → **Ordinal_71** + `FUN_004e79d0` / `FUN_004d75b0` (worker cmd 0x0D) | `FUN_101a9460` @ `0x101a9460` / internal TSDev | **MISSING** |
| 14c | `FUN_004c9670` → **Ordinal_68** (dependent list + count) | `FUN_101a87d0` @ `0x101a87d0` | **MISSING** (CLI uses its own enumeration) |
| 14d | Recursive `FUN_004D91E0` per dependent | internal TSDev | CLI uses `traverseReverseDependents` (different mechanism; skipped for cat-0 roots by default — see §13 "CLI implementation") |
| 17 | **Ordinal_17** `(category, progId)` | `FUN_10151be0` @ `0x10151be0` | **MISSING** |
| 19 | `FUN_004531A0` (property/cache persistence) | internal TSDev | **MISSING** |
| — | **Ordinal_57** `(category, progId)` via `FUN_00453f70` | `FUN_101526f0` @ `0x101526f0` | ✓ (cross-process to TradeStation processes via shared `SHARE_WO` array) |

### Category gate in `FUN_004D91E0`

```c
if ((uVar6 == 1) && (((uVar2 == 1 || (uVar2 == 4)) || (uVar2 == 0))))
```

The post-verify dependent-recalc block (Ordinal_10, FUN_004D7140, worker cmd 0x0D, FUN_004c9670, recursive traversal) executes **only** for categories 0 (Strategy), 1 (Function subtype), and 4 (Function). For other supported types (Indicator cat=2, ShowMe cat=2 subType 0x05, TradingApp cat=2 subType 0x14), the block is skipped — the shared-warehouse path is sufficient because the modified item IS the applied item.

### Ordinal_25 / Ordinal_17 — pending-save queue in SHARE_WO

- `Ordinal_25` (`FUN_10151b10` @ `0x10151b10`): adds `(category, progId, name)` to a shared array at `DAT_10243550` (inside `SHARE_WO`), max 499 entries, each 0x45 bytes: `[category:int32][progId:int32][name:char[0x3D]]`. Count at `DAT_10488828`.
- `Ordinal_17` (`FUN_10151be0` @ `0x10151be0`): removes the entry.
- `Ordinal_22` (`FUN_10151c60` @ `0x10151c60`) / `Ordinal_23` (`FUN_10151cd0` @ `0x10151cd0`): read the queue (called internally by orlib20's compile path `FUN_10168ee0`).
- No evidence that orchart.exe polls this queue; it may be purely for orlib20's compile-time name resolution.

### Ordinal_56 — flag reset

`Ordinal_56` (`FUN_10158b70` @ `0x10158b70`): sets `DAT_1023aac8 = 0` (in orlib20's private `.data`, per-process). Checked at end of `FUN_10168ee0`. May control a compile-time behavior switch.

---

## 13. Function-modify recalc mechanism (RUNTIME CONFIRMED) + recalc matrix

### Confirmed mechanism

The cross-process recalc mechanism for function-modify (and all dependent-recalc scenarios) is:

1. **Worker cmd 0x0D** (FindProgram) → COM call to WHServer.exe → fires `_IWHouseEvents` COM events → orchart receives and recalculates.
2. **Ordinal_57** (0x40A notification) → posts `PostMessageW(0x40A, ...)` to orchart HWNDs via the shared `SHARE_WO` array → orchart processes the notification.

Both are needed: 0x40A delivery alone was confirmed insufficient. Cmd 0x0D is the necessary additional trigger that fires the WHServer.exe COM events causing orchart to recalc. The CLI performs both, matching TSDev.exe's post-verify sequence.

Evidence:
- **0x40A alone is insufficient:** Before the cmd 0x0D dispatch was added, the CLI already posted 0x40A to all 19 subscriber HWNDs (via the shared `SHARE_WO` array), but the applied caller indicator did NOT auto-recalc — it updated only after a ~13 s delay (manual/periodic refresh). Runtime-confirmed via Frida: 16 orchart `DispatchMessageW` arrivals for `PostMessageW(0x40A, 0, 0x10054)`, but no immediate recalc.
- **cmd 0x0D closes the gap:** After adding the cmd 0x0D dispatch to `verifyModule()`, CLI `modify function TsDeskFnProbe_20260829` (ProgId 84, cat 1) v2→v1 from elevated direct CLI, exit 0. Debug log: `verifyModule: dispatching post-verify cmd 0x0D cat=1 progId=84` and `verifyModule: post-verify cmd 0x0D OK, 0x5E=0x... (cat=1 progId=84)` (valid IProgram pointer — FindProgram succeeded). User-confirmed: applied caller indicator `TsDeskFnProbeCaller_20260829` (ProgId 3008) auto-recalculated immediately (2→1) without manual refresh. No crashes.
- **WHServer.exe COM event count diff:** Runtime Frida trace comparison — TSDev UI save fired ~468 WHServer.exe COM events (`NdrStubCall2` + `NdrClientCall2` burst); CLI modify (before cmd 0x0D) fired ~141 (~327 fewer, primarily from the missing post-verify block). The extra events are from the cmd 0x0D → WHServer.exe COM path.

### CLI implementation

The CLI dispatches cmd 0x0D via the generic dispatcher (`FUN_101cf5b0` via `callCmdDispatcher`) inside the category-gated (`rootCategory == 0 || 1 || 4`) post-success block in `verifyModule()`, after `linkModule` and before `traverseReverseDependents`. No Ordinal_10/71/68 or worker-thread infrastructure replication was needed. Non-fatal on all failure modes.

The full TSDev post-verify block also includes Ordinal_10 (session get), Ordinal_71 (singleton), Ordinal_68 (dependent count), and a recursive per-dependent `FUN_004D91E0` traversal. These were NOT needed for the single-caller probe. For multi-dependent scenarios, the recursive traversal with per-dependent cmd 0x0D may be needed — potential future TVS if multi-dependent recalc is found insufficient.

**Strategy (cat-0) roots:** `verifyModule()` skips the master-record contract-check and `traverseReverseDependents` for category-0 roots by default (the post-verify `linkModule` and cmd 0x0D dispatch still run unconditionally within the 0/1/4 gate). A strategy cannot be a dependency target — dep-list entries (0xFA3/0xFA4) record FUNCTION references only and TSDev's dependent query returns empty for strategy roots — so the per-item whole-library scan was provably vacuous for cat-0 while being the dominant WHServer VAS consumer. The legacy traversal is restorable via `--traverse-strategy-dependents` / `TSDEV_TRAVERSE_STRATEGY_DEPENDENTS` (any value but `"0"`); under `TSDEV_DIAG` the skip emits `traverse.skipped rootCat=0 rootProgId=N reason=strategy-root`.

### Recalc matrix (ALL 8 ENTRIES CONFIRMED — CLOSED)

Each entry confirmed by CLI modify from elevated direct CLI (exit 0) + user confirmation of immediate recalc (no manual refresh) + no crashes/popups. Debug log lines cited where cmd 0x0D was dispatched.

| Scenario | Mechanism | Evidence |
|---|---|---|
| Direct-applied Indicator (cat 2) modify | Shared-warehouse writes + 0x40A (no cmd 0x0D — cat 2 gate) | CLI-inserted indicator `TsDeskProbe_20260829123929` (ProgId=2996) applied to chart; CLI modify → immediately visible/plotted; CLI delete (while applied) → auto-removed from chart + Insert Analysis Techniques list |
| Direct-applied ShowMe (cat 2, subType 0x05) modify | Shared-warehouse writes + 0x40A (no cmd 0x0D — cat 2 gate) | ShowMe `TsDeskShowMeProbe_20260830` (ProgId=3026) v1→v2 (High→Low markers); debug log: NO cmd 0x0D (correct for cat 2); user-confirmed markers moved immediately |
| Direct-applied Strategy (cat 0) modify | cmd 0x0D (cat 0) + 0x40A | Strategy `TsDeskStrategyProbe_20260829` (ProgId=9042) v1→v2; debug log: `post-verify cmd 0x0D OK, 0x5E=00C254A4` (cat=0 progId=9042); user-confirmed signal labels V1→V2 immediately |
| Direct-applied TradingApp (cat 2, subType 0x14) modify | Shared-warehouse writes + 0x40A (no cmd 0x0D — cat 2 gate); open WinForms form reinitializes | TradingApp `TsDeskTradingAppProbe_20260830` (ProgId=3030) v1→v2; debug log: NO cmd 0x0D (correct for cat 2); user-confirmed open form reinitialized V1→V2 immediately |
| Function → Indicator caller modify | cmd 0x0D (cat 1/function) + 0x40A | Function `TsDeskFnProbe_20260829` (ProgId=84) v2→v1; debug log: `post-verify cmd 0x0D OK, 0x5E=0x... (cat=1 progId=84)`; user-confirmed applied caller indicator `TsDeskFnProbeCaller_20260829` (ProgId=3008) recalculated 2→1 immediately |
| Function → ShowMe caller modify | cmd 0x0D (cat 1/function) + 0x40A | Function `TsDeskFnProbe_20260829` (ProgId=84) v1→v2; debug log: cmd 0x0D dispatched (cat=1 progId=84); `enumerateReverseDependents` reached ShowMe (cat=2 pid=3029); user-confirmed markers moved High→Low immediately |
| Function → Strategy caller modify | cmd 0x0D (cat 1/function) + 0x40A | Function `TsDeskFnProbe_20260829` (ProgId=84) v1→v2; debug log: `post-verify cmd 0x0D OK, 0x5E=008D3154` (cat=1 progId=84); user-confirmed applied Strategy signal labels `STFN_V1`→`STFN_V2` immediately |
| Function → TradingApp caller modify | cmd 0x0D (cat 1/function) + 0x40A; open WinForms form reinitializes | Function `TsDeskFnProbe_20260829` (ProgId=84) v1→v2; debug log: `post-verify cmd 0x0D OK, 0x5E=0E8B924C` (cat=1 progId=84) + `enumerateReverseDependents: cat=2 pid=3032`; user-confirmed open TradingApp form reinitialized V1→V2 immediately |

---

## 14. TSCrash.dll unhandled-exception filter — TSCrashReport.exe parenting

TSCrash.dll's unhandled-exception filter launches `TSCrashReport.exe "<faulting-exe-basename>" [SILENT]` via `CreateProcessW` **on the crashing thread** and then blocks on `WaitForSingleObject(child, INFINITE)`. Therefore:

- (a) `CreateProcessW` runs on the crashing thread, so the reporter's `ParentProcessId` is the crashed process's pid.
- (b) `WaitForSingleObject(child, INFINITE)` keeps the crashed process frozen-but-alive for the reporter's entire lifetime. In-field: WHServer pid 19532 remained enumerable with `present=1` in `whserver.sample` for ≥45 s post-crash (`dist_logs/tsdev_diag_45356_20260924_080015.log`).

Evidence: TSCrash.dll disassembly — `CreateProcessW` call sites at TSCrash+0x2D7B/+0x34F9, the command-line build at +0x33F3–+0x349F verified verbatim; the command line carries the basename only, no pid. Full analysis: `tmp/whserver_crash_analysis/report.md`; disasm evidence scripts: `tmp/crashrep_strings/disasm_cp*.py` (+ the analysis conversation).

Consequence for the dispatch watchdog (`src/dispatch_watchdog.cpp`): a `TSCrashReport.exe` whose parent pid is a WHServer.exe pid is a proven "WHServer crashed and is frozen" signal — the wedged COM call into it will never return — while the crashed WHServer's pid still enumerates as present (so presence alone is not proof of health).

---

## 15. WHServer.exe VAS exhaustion — `std::bad_alloc` crash mechanism (PROVEN)

- **WHServer.exe is 32-bit non-LARGEADDRESSAWARE** (CHARACTERISTICS `0x103`, no `0x20` bit) → hard **2 GB user-VAS ceiling** for the shared warehouse server. [PROVEN: PE header inspection]
- **Observed fatal failure:** WHServer pid 19532 threw unhandled `std::bad_alloc` at 09-24 08:00:36 with **2047 MB used / 10 MB free** user VAS. CRT path: `operator new` (`0x42D150`) → `malloc` NULL → `_callnewh` → `__scrt_throw_std_bad_alloc` (`0x42DB87`). Handle/GDI counts were normal — pure virtual-address-space commit exhaustion, not a resource leak. [PROVEN: `_ThrowInfo` RTTI `.?AVbad_alloc@std@@` + TSErrorLog header]
- **VAS exhaustion is cumulative across all attached clients.** WHServer is the lazy-lifetime singleton shared by tsdev_cli, TSDev.exe, TSELAppHost.exe, orchart.exe, etc.; pid 19532 had served our batch runs since 09-20 plus concurrent desktop traffic. Which client's workload filled the 2 GB is not attributable from available evidence. [INFERRED]
- **The throw site is an incoming COM read, not our specific call:** STA dispatch → selector `0x5DC` → `WHServer!0x415D90`, a binary-blob getter materializing item binary content into `SAFEARRAY(VT_UI1)` (`new[blob_count]` + memcpy in `WHServer!0x40CC80`; needs ~2× blob bytes incl. the SafeArray data block). Once VAS is ~exhausted, *any* server-side allocation can fail — the crash lands at a random later call. [PROVEN shape; symbolic name INFERRED]
- **Client-visible cascade:** TSCrashReport froze WHServer (§14(b)) → pending ORPC never returns → our `CCliModalLoop` infinite wait; other clients receive `_com_error(0x80010105 RPC_E_SERVERFAULT)` on orlib20 worker threads; TSDev.exe crashed 6 s later (`0x80000003` in the cmd-0x35 dispatch family). [PROVEN: TSELAppHost minidumps + aggregated report blocks]
- **Pre-fatal manifestation [INFERRED]:** a WHServer approaching the ceiling can manifest as slowness/wedge (new-handler retries, fragmented commits) before `malloc` returns NULL — the 08:04/10:08 wedge-only hangs are plausibly this mechanism, unproven.

Evidence: `tmp/whserver_crash_analysis/report.md` (analysis of `D:\protege_strategy_dev\tsdev_automation_dist_diag\TS_Crash_files`; ThrowInfo decode, disasm listings). Correlating root-cause report: `reports/2026-09-29_dist_logs_crash_analysis.md` postscript.

## 16. `Program\Cache` backup/restore mechanics (tsdev_cli --backup-cache/--restore-cache)

- **TS Desktop holds `Program\Cache` files open deny-read.** Copying a live cache fails `ERROR_SHARING_VIOLATION` (err=32) on `CAL_cache.clod`, `Holidays.dat`, `Sessions.dat`, etc. [PROVEN: live run]
- **`FILE_FLAG_BACKUP_SEMANTICS` does NOT bypass share locks.** It overrides ACL/security checks (with `SE_BACKUP_NAME`, present when elevated) but share-mode conflicts are enforced by the I/O manager regardless of privilege in user mode. Only VSS reads past deny-read. [PROVEN: OSR/MSDN refs + live err=32]
- **In-proc VSS (`IVssBackupComponents`/vssapi) cannot work from a WOW64 process** — `CoCreateInstance(CLSID_VSSCoordinator)` = `REGDB_E_CLASSNOTREG` (0x80040154); VSS requesters are 64-bit-only on x64. [PROVEN: live]
- **Direct WMI COM (`Win32_ShadowCopy.Create` via `IWbemServices`) also refuses WOW64 clients**: `ExecMethod` -> 0x80041014 (`WBEM_E_PROVIDER_NOT_FOUND`); the shadow-copy method provider is 64-bit-only; even 32-bit powershell `-ComputerName localhost` DCOM fails ("Initialization failure"). [PROVEN: live]
- **Working 32-bit to 64-bit bridge: spawn 64-bit `powershell.exe` via `%windir%\Sysnative`** (WOW64-only redirect to System32) with `-EncodedCommand` running `Win32_ShadowCopy.Create`/`Delete` — no new dependencies; needs privileges ENABLED on the caller token (`SE_BACKUP_NAME`/`SE_RESTORE_NAME`; the provider impersonates it — bare `ReturnValue=5` otherwise). The shadow's `DeviceObject` (`\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN`) is then readable from the 32-bit process with plain `CopyFileExW`. [PROVEN: group-19 test — real shadow create+release, byte-identical capture of a deny-all-locked file]
- **Share-lock probes:** access-0/share-0 opens are BLIND to deny-all handles (they succeed); `DELETE` desired-access + share-0 correctly detects lock classes (err=32) while passing readonly files. [PROVEN: empirical probes]
- **NTFS in-place compression** via `FSCTL_SET_COMPRESSION`/`COMPRESSION_FORMAT_LZNT1` is per-file atomic and transparent to readers — safe on a detached thread that may die mid-file; needs no admin beyond normal write access. Observed ~16:1 on compressible fixtures; `CAL_cache.clod`-class binary data compresses substantially less — treat as opportunistic savings, not a guarantee.

## 17. WHServer.exe IWHouse COM activation chain (PROVEN end-to-end, live)

The WHouse coclass `{7CEEA907-D59E-11D3-8394-00C04F7BDF10}` is **not registered** (`CoCreateInstance` = `REGDB_E_CLASSNOTREG`), and WHServer is not in the ROT (0 monikers). Activation goes through the registered **Control** coclass and a custom licensed factory interface:

1. `CoGetClassObject(CLSID_Control {7CEEA90C-D59E-11D3-8394-00C04F7BDF10}, CLSCTX_ALL, NULL, IID {7CEEACBC-D59E-11D3-8394-00C04F7BDF10}, &factory)` -> S_OK. The factory IID `{7CEEACBC}` is unregistered yet marshals (WHServer serves it from its class-object table; an earlier mistaken transcription `{7CEEAEBC}` returns `E_NOINTERFACE`). orlib20: `FUN_101ec100` @ `0x101ec100`.
2. `factory->vtable[8]` (offset `0x20`), signature `HRESULT fn(this, u4 a0=0, u4 a1=0, const IID* riid, BSTR name, void** outObj, BSTR* outName)`; called with `riid=&IID_IControl {7CEEA90A}`, name `"<268435478>,<ror(((GetTickCount>>12)&0xFFFF|0x160000)^0x1256abef,6)>"` (first field is the constant `0x10000016`). Returns the **IControl** proxy plus an assigned out-name `"268435479,�"`. [PROVEN: Frida capture of orlib20 worker tid + verbatim `tmp\wh_call.exe` run]
3. `IControl::WHouseEx` (dispid `0x01`, **DISPATCH_PROPERTYGET**, params `(BSTR DirName, I4 Create)` -> `VT_DISPATCH`) -> **IWHouse** proxy. orlib20 does the equivalent via `FUN_101cf170` = `control->QueryInterface(IID_IWHouse {7CEEA903})` on the object produced by the factory; `WHouseEx` on the returned Control object is the documented property-get equivalent. [PROVEN: live `DISPATCH_PROPERTYGET` Invoke S_OK, returned object QI/typeinfo = `IWHouse` `cFuncs=58`]
4. Direct `CoCreateInstance(CLSID_Control)` = `REGDB_E_CLASSNOTLICENSED` (`0x80040112`) � the class object only issues instances through the `{7CEEACBC}` factory path.
5. Warehouse slots written on the worker STA (`FUN_101d2150`, `CoInitialize(NULL)`): `+0xAA` = IControl proxy, `+0xAE` = IWHouse proxy, `+0xC6` = `CELWHouseEvents` sink advised via `AfxConnectionAdvise`. QI on those proxies from any other apartment -> `RPC_E_WRONG_THREAD` (`0x8001010E`); callers must go through the worker dispatcher, or create their own IWHouse via the same chain (each client may bind its own reference).

IWHouse maintenance members verified live (typelib `IWHouse`, `WHServer.exe` type library `{7CEEA906}`): `FlushCache` dispid `0x18`, `ClearVerifyFlagByType` dispid `0x1F` (1 param, VT_I2 = warehouse category), `ClearDependencyLists` dispid `0x20`, `ReleaseRemainingPrograms` dispid `0x30` � all invoked `DISPATCH_METHOD` -> `S_OK` on the live server with WHServer pid stable and no measurable VAS delta at a cold cache. `ForceShutDown` is dispid `0x2B` � called twice live via `tmp\wh_shutdown.exe`, both S_OK, but the server stayed alive >=7.5 min under a running desktop (idle-gated exit � see �18).

`IControl` typelib: `cFuncs=8`, sole extra member `WHouseEx` (memid `0x01`, invkind=PROPERTYGET, `(BSTR,I4)->VT_DISPATCH`).

Evidence: `tmp\wh_com_trace.js`/`tmp\run_trace.py` Frida capture (`wh_com_trace.log`), `tmp\wh_call.cpp` client, run output `tmp\wh_call_run.txt`, Ghidra `FUN_101ec100`/`FUN_101d2150`/`FUN_101cf170`/`FUN_101d1a90` in orlib20.dll.

## 18. WHServer.exe lifecycle, maintenance surface & recovery findings (2026-10-18)

Durable facts from the `whserver_vas_free` recovery research. Full report: `reports/2026-10-18_whserver_recovery_research.md`; live ledger: `docs/WORKING_MEMORY.md`.

- **Server lifecycle model:** `WHServer.exe -Embedding` -> `CoRegisterClassObject(REGCLS_MULTIPLEUSE)` -> message loop. A MonitorShutdown thread exits the process only after **5 s of `m_nLock==0`** � idle-exit is lock-driven, not a fixed timer. Observed: ~24.5 min linger under held locks; transient servers self-reaped <17 s idle; a broker-activated server lingered 50 s+ after broker death (lock rundown lag). [PROVEN: Ghidra WinMain/MonitorShutdown disasm + live watches]
- **SCM respawn semantics:** WHServer is spawned on demand by DCOM SCM (parent `svchost -k DcomLaunch`, `-Embedding`). After full death the next activation gets a fresh server � measured ~0.4 s from `tsdev_cli exists` to serving; fresh server VAS = 1899 MB free vs ~1003 MB pre-kill warm, and a broker-warmed server settles ~1105 MB free (orlib20 eager-commits ~1 GB at attach). New client processes provably get fresh servers (x4+). A TSCrashReport-frozen zombie still enumerates and gets reactivated � ensure full death before reactivation. [PROVEN: live]
- **Maintenance surface is callable but cannot reset VAS:** `FlushCache`/`ClearDependencyLists`/`ClearVerifyFlagByType`/`ReleaseRemainingPrograms` all return S_OK over the �17 chain. `ReleaseRemainingPrograms` (dispid `0x30`; also called by orlib20 `closeWarehouse` teardown) Releases every cached per-type record yet reclaimed +0.00 MB committed VAS even with a ~17 MB warm cache � WHServer uses CRT `malloc`/`free` only (no `HeapCreate`/`VirtualAlloc` imports), so freed objects stay in committed heap pages. [PROVEN: `tmp\wh_call_run.txt`, `tmp\wh_call_run_warm.txt`, `tmp\wh_call_run_warm2.txt`]
- **`ForceShutDown` (dispid `0x2B`) verified live � INERT under attached clients:** called twice, both S_OK, but WHServer pid 2956 stayed alive >=7.5 min still serving new activations � attached clients keep re-locking so the 5 s zero-lock window never occurs. Graceful server exit is unreachable while any client calls. [PROVEN: `tmp\exp_watch.log` (pid alive through 287 s watch), `tmp\exp_pre.txt`/`tmp\exp_post.txt` bookend ~7.6 min]
- **Hard kill under a running desktop = functional blast radius:** `taskkill /F` kills WHServer in <1 s with no TSCrashReport child (clean kill). Zero of 15 watched TS client processes died in a 7 min watch � BUT the user observed a ~2 min desktop UI stall matching the kill window (ORPC-blocked clients), and field evidence (�15) shows attached callers die on their next COM call. Process-liveness "zero casualties" undercounts functional blast radius; hard kill is safe only when no foreign orlib20 clients are running. [PROVEN: `tmp\exp_watch_kill.ps1` watch + user observation]
- **Census lesson � module-loaded != COM-attached:** 11 of 13 TS-name-matched processes had orlib20/OREL modules loaded yet were idle-detached; the desktop still froze during the kill window, so clients did call the warehouse in-window (or had calls in flight). Any "safe to recycle" gate must detect ACTIVE attachment, not module presence. WHServer itself does not load orlib20 (it uses `orgcom20.dll`+`orshr20.dll`). [PROVEN: `tmpas_probe_report.txt` TASK 1]
- **Per-op server VAS footprint:** single exists/insert/verify/delete ops cost ~0 MB measurable free-VAS delta (0.01 MB resolution); ~30 MB one-time first-contact reservation per fresh server. Exhaustion is cumulative across ops and all clients. [PROVEN: `tmpas_probe_report.txt`]
- **`TSWAREHOUSEMUTEX`** = likely "WHouse in memory" mutex token [INFERRED]; `IWHouse::SetProcessId`/`DumpProcessAttached` are STUBS � no server-side PID table exists to enumerate attached clients [PROVEN: Ghidra].
- **Reconnect mechanics (orlib20):** zero reconnect code (no server-death HRESULT handling); a dead server = permanently broken `CELWHouse` until close. `closeWarehouse` destroys `CELWHouse` (calls `IWHouse::ReleaseRemainingPrograms` + releases proxies); the next warehouse call builds a fresh `CELWHouse` -> `CoGetClassObject` -> SCM respawn if dead. In-process reuse after close is unsafe (null-deref in cmd `0x4E`), so reconnect effectively means a new process/broker. [PROVEN: Ghidra + prior field evidence]
- **Net:** the only VAS reset is a server restart (kill + SCM respawn); with the desktop up there is no clean recovery path. Fix direction = root cause: match TSDev's per-op server footprint (user evidence: TSDev-driven automation runs 500-1000+ inserts/session with no VAS problems). [PROVEN facts; direction is the recommendation]

## 19. WHServer deferred-commit / backlog-drain model (2026-10-19)

Findings detail: `tmp\P1_FLUSH_DRAIN_FINDINGS.md`, `tmp\P1R2_BACKLOG_FINDINGS.md`, `tmp\P1R2B_FINDINGS.md` (+ P1R3 cells).

- **Deferred commits reproduce on a warmed server.** Verbatim BIG40K op-mix replay on a ~13-16 min, ~40-session WHServer produced a 15-record deferred burst (~0.31-0.49 s/record under tracing). [PROVEN: P1R2 a3f, P1R2B arms G/H traces]
- **Drain vehicle = age-gated housekeeping pulses** (`ReleaseRemainingPrograms`, dispid 0x30, on the housekeeping/RPC thread). Best-fit rule: **the first pulse after the YOUNGEST dirty record reaches ~60 s drains the whole backlog aggregate-style** (a3f 150.6 s pulse skipped 65.8-73.4 s-old records -> "~60-70 s" unsupported; a3f bounds modify-gate ~73-92 s). [INFERRED best-fit - CONTRADICTED by P1R3: cell C's drain attached at youngest ~49.1 s (C05 committed at ~59.07 s, under 60); cell A's op-free pulse at rel 220.247 ran with youngest ~65.3 s and committed nothing; P1R3 concludes the gating mechanism is UNRESOLVED]
- **Pulses are NOT a heartbeat:** cadence 19.7-116.1 s observed (standalone gaps across P1R2B + P1R3); they ride an UNIDENTIFIED background enumerator - a ~2,300 impl-calls/s scan loop (get_ProgramById walks + ProgramsCountByType tails + per-program QI churn) that runs during silent windows and stops at drain. It is NOT backlog-conditioned as first stated: the same churn ran in P1R2B arm-I windows with NO backlog (i-trace rel 115.8-134.8 and 141.2-160). Conditioning (what starts/stops it) AND driver (foreign client vs server-internal vs held-session) are BOTH UNDISCRIMINATED. [PROVEN scan loop; conditioning+driver OPEN]
- **Session requirement:** commits need a LIVE SERVER INSTANCE running a pulse; a held session is NOT required (P1R3 cell A drained post-release with zero held sessions, n=1). In R2b a keep-alive session was always held so those arms alone could not discriminate. Which session hosts a burst is incidental (9/15 of a3f burst inside the +75 s probe, 6/15 after exit). [PROVEN for live-instance requirement; held-session-not-required n=1]
- **Dirt-type discriminator CONFOUNDED** (wave order / cumulative state / warmth covary): operative contrast = single-op creates commit early on post-exit pulses (~4-26 s, never inside the op window) vs modify/batch ops gate at ~60 s+; batch-inserted records ARE creates (CreateProgram fires) yet defer - phrase as single-op vs batch/modify. Cross-run variance unresolved: BIG40K saw single inserts commit in-session pre-exit 5/5; P1R2 saw post-exit-pulse commits. [INFERRED/OPEN]
- **Deletes are synchronous in-session** (`RemoveProgram` impl 0x41e480): P1R2B arm I (5x 720B fresh) and P1R3 cell C (5x ~40KB) - contradicts BIG40K's "deferred deletes" inference (RemoveProgram never observed on its real 40KB items); both stand with their scopes. [PROVEN for tested scope]
- **`IWHouse::FlushCache` is FALSIFIED as a drain lever in every executed condition** (P1 arm flushes no-op on empty backlog; R2B G queued ~50 s to cascade tail; R2B H backlog pre-drained; R3 cell A never dispatched amid churn). The synchronous-flush-on-undrained-backlog cell is formally UNTESTED. Instrument contract validated: impl FUN_0041ccd0 takes only ctx (RET 0x4), zero-arg Invoke = orlib20's native vtable+0x80 call; trace a1/a2 = stack residue. [PROVEN]
- **Instrumented VAS is an artifact:** Frida send-queue inflation shows ~800 MB phantom usage; instrumented freeMB numbers (e.g. 268-290) are artifacts - trust post-detach values only (~1096-1902 MB). [PROVEN]
- **Residual risk:** deferred bursts run the BlobGet-heavy crash-site pattern (section 15) uncontrolled and outside any guarded client call; the VAS guard cannot see them; the poisoning vector is unchanged. Exit-14 refusals still occur. [INFERRED consequence of PROVEN facts]

## 20. WHServer m_nLock census gate (2026-10-19)

Full reports: `tmp\P2_CENSUS_LOCK_PROBE.md` (phases 1-4, lock-delta + self-reap evidence), `tmp\P2_DESKTOP_WATCH.md` (25-min idle-desktop watch).

- **`_Module.m_nLock` is a readable foreign-client census counter:** DWORD at fixed VA `0x0043F8A8` in WHServer.exe (`m_bActivity` byte at `0x0043F8D8`); readable externally via `OpenProcess(VM_READ|QUERY)` + `ReadProcessMemory`. Semantics = count of client-held COM objects, not sessions: +9 at quiescence for a passive WHouseEx hold, +6 for a paused batch session, transient per-op ramps to ~422 while an op is in flight. [PROVEN: Ghidra + live sampling]
- **Graceful release drops lock to 0 in <0.5 s** (three observations: ~0.65 s peak-to-0 max, last-nonzero-to-0 ~0.22 s (leg: 20@28.307 -> 0@28.527; the ~0.435 s figure covers the wider 232@28.092 -> 0 leg), <422 ms). Self-reap: process exits ~6 s after sustained lock==0 (6.0 / 6.3 / 5.9 s); MonitorShutdown sets `m_bActivity` at ~5.0 s and exits only after 5 s of sustained 0. [PROVEN: live]
- **Census gate shape [PROVEN]:** `WHServer process-absent OR m_nLock==0 sustained >=~3 s`. The ~3 s floor clears the max observed benign between-op zero-gap (2.057 s; true window <=~2.49 s) while staying under the ~5 s monitor window. Alternative arm (also passes during self-reap lag): `process-absent OR exits within ~7 s`. Process-absent passes only while the desktop is verified up.
- **Recycle-without-kill path:** `ForceShutDown` (dispid `0x2B`) -> zero locks + class-object revoke -> SCM respawn on next activation = recycle without terminating the pid. ForceShutDown is inert while attached clients keep re-locking (>=7.5 min live observation, sections 17/18). [PROVEN static + inert-under-desktop live]
- **Fail-open residual = class-factory TOCTOU:** a client holding only a class-factory proxy contributes 0 to m_nLock yet can become a caller instantly; sustained-0 + revoke-then-respawn narrows but does not eliminate the race. [PROVEN]
- **Abrupt-client-death rundown lag:** an ungraceful client death may hold lock>0 for tens of seconds (prior 50 s+ broker-death observation, provenance of graceful-vs-abrupt unknown). Fail-closed direction only. [UNTESTED]
- **Foreign floor under a live desktop:** 25.0-min sustained watch (491 checks, ~3 s cadence, max inter-sample gap 3.443 s < ~5 s min server lifetime - no spawn+death can hide) - WHServer.exe absent at every sampled instant of an idle evening desktop; zero spawns. [PROVEN, scoped to idle desktop] The operationally relevant case - desktop sustaining a WHServer our own ops spawned - was not exercised. [UNTESTED]

## 21. WHServer m_nLock process-lifetime refs and kill semantics (2026-10-21)

Live-proven on this machine during the gated-recycle end-to-end verification (desktop closed).

- **An attached CELWHouse client contributes +7 to `m_nLock` for its ENTIRE process lifetime.** The +7 = activation/factory references held since attach; `closeWarehouse` does NOT release them. Only process exit drops them. Live-measured: lock=7 constant through an attached process's whole census; control-verified that a graceful process exit releases them (lock->0 at the exit instant; WHServer self-reaps ~6 s later via MonitorShutdown). [PROVEN: live]
- **TerminateProcess orphans the +7:** a client killed via TerminateProcess leaves its +7 registered on WHServer for >=5.6 min (observed floor; possibly longer) - the COM release RPCs never run and ORPC rundown does not arrive on census timescales, so any census reads `census_lock` indefinitely. Explains the earlier long-lived WHServer linger cases (sections 18/20). [PROVEN: live]
- **Consequence for the recycle census:** the engine census can NEVER pass inside an attached process - the process's own +7 always counts. The engine is only meaningful at a never-attached site (own contribution 0). Shipped rule: `recycleSiteDefers` in `src\tsdev_cli.cpp`. [PROVEN: live]
- **ForceShutDown live signature:** `m_nLock` reads 0, then briefly `0xFFFFFFF9` (-7, the forced negation), then the server dies. Distinguishes a real ForceShutDown kill from a self-reap (0 -> READ_FAIL, no negative phase). [PROVEN: live]
