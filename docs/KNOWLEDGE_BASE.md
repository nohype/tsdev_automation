# TSDev Knowledge Base

Current proven knowledge only. Every statement cites evidence. No assumptions, conclusions, history, or unevidenced claims.
No decision record.

---

## 1. Repository structure

| Path | Purpose | Evidence |
|------|---------|----------|
| `src/tsdev_lib.cpp`, `src/tsdev_lib.h` | Main C++ library wrapping orlib20.dll | file listing |
| `src/orlib_resolver.cpp`, `src/orlib_resolver.h` | Signature-based resolver for orlib20 internal functions | file listing |
| `src/tsdev_cli.cpp` | Production CLI (`tsdev_cli.exe`) | file listing |
| `src/tsdev_automation/` | Python wrapper (`TsDevClient`) | file listing |
| `signatures/v9.50.01.3344.json` | Signature definitions for orlib20 v9.50.01.3344 | file listing |
| `signatures/version_manifest.json` | Version manifest | file listing |
| `tools/sigcodegen.py` | Signature code generator | file listing |
| `tests/` | C++ unit tests and Python tests | file listing |
| `AGENTS.md` | Mandatory project rules | file listing |

---

## 2. orlib20.dll basics

- Base address: `0x10000000` (Ghidra project `TSDevAnalysis`)
- Target version: v9.50.01.3344 (`signatures/v9.50.01.3344.json`)
- 32-bit DLL; CLI and all callers must be 32-bit (`AGENTS.md`)
- Loaded from `C:\Program Files (x86)\TradeStation 9.5\Program\` (registry-resolved, `src/tsdev_cli.cpp`)

---

## 3. Warehouse memory layout

The warehouse is a singleton obtained via `GetWarehouse(NULL)`. Resolved as `m_pGetWarehouse` in the CLI. All offsets are relative to the warehouse base pointer.

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

## 4. Worker thread command dispatch

### Generic dispatcher: `FUN_101cf5b0` (RVA `0x1cf5b0`, abs `0x101cf5b0`)

`__thiscall`: ECX = warehouse, stack arg = cmd number.

Sequence (Ghidra decompilation):
1. `WaitForSingleObject([warehouse+0x72], INFINITE)` — acquire mutex
2. Thread-affinity check: `GetCurrentThreadId() != [warehouse+0x76]` and cmd != 0x1E → `swi(3)` (crash)
3. `[warehouse+0x04] = cmd`
4. `SetEvent([warehouse+0x6E])` — wake worker
5. `WaitForSingleObject([warehouse+0x6A], INFINITE)` — wait for completion
6. `ReleaseMutex([warehouse+0x72])`

CLI wrapper: `callCmdDispatcher(m_pCmdDispatcher, warehouse, cmd)` at `src/tsdev_lib.cpp`.

### Worker thread: `FUN_101d2150` (RVA `0x1d2150`, abs `0x101d2150`)

Reads `[warehouse+0x04]`, decrements by 1, bounds-checks against `0x79`, jumps through table at `0x101d3680` (`JMP dword ptr [EAX*4 + 0x101d3680]`).

---

## 5. Commands

### cmd 0x0D — FindProgram

- Worker handler at `0x101d2800` (Ghidra disassembly)
- Calls `programs_collection->vtable[0x20]` (FindProgram) with `(whType=[+0x12], name=[+0x16], &out_IProgram)`
- On success: `[warehouse+0x5E] = IProgram` (record-reference)
- On failure: `[warehouse+0x5E] = 0` (at `0x101d285d`)
- The dispatcher `FUN_101cfce0` (RVA `0x1cfce0`) reads `[+0x5E]` and immediately clears it (borrow pattern): `uVar3 = *(warehouse+0x5E); *(warehouse+0x5E) = 0; return uVar3;`
- The IProgram from cmd 0x0D is a record-reference — its caller/dependency tables are not populated

### cmd 0x0F — LoadProgram

- Worker handler at `0x101d2891` (Ghidra disassembly)
- Calls `programs_collection->vtable[0xA8]` (LoadProgram) with `(16-byte struct from [+0x1E], whType=[+0x12], &out_IProgram)`
- On success: `[warehouse+0x5E] = IProgram` (fully-loaded)
- On failure: `[warehouse+0x5E] = 0` (at `0x101d285d`)
- The IProgram from cmd 0x0F is fully-loaded — its caller/dependency tables are populated by the load
- **Must be dispatched via specialized `FUN_101d0950`, not generic `FUN_101cf5b0`** — the generic dispatcher does not set up the 16-byte load struct at `+0x1E` and returns `E_INVALIDARG` (`0x80070057`). Proven by dynamic test (this session): generic dispatch of 0x0F returned status `-2147024809` (`0x80070057`), `warehouse+0x5E = NULL`, followed by crash.

### cmd 0x3B — IProgram::Callers

- Worker handler at `0x101d3156` (Ghidra disassembly)
- `MOV ECX, [EDI+0x5E]` — reads IProgram from `warehouse+0x5E` directly (**no NULL check**)
- `LEA EAX, [EDI+0x1E]` — out-param is `&warehouse+0x1E`
- `MOV EDX, [ECX]; CALL [EDX+0x38]` — calls `IProgram->vtable[0x38]` (Callers)
- `MOV [EDI+0x4E], EAX` — HRESULT to `warehouse+0x4E`
- SafeArray pointer lands at `warehouse+0x26` (VARIANT value field at `+0x1E+8`)
- **NULL `+0x5E` causes access violation** at `MOV EDX, [ECX]` (dereferences NULL). Proven by dynamic test (this session): crash + `TSCrashReport.exe` dialog.

### cmd 0x4E — Cache refresh

- Dispatched by `refreshCache()` at `src/tsdev_lib.cpp:2184` via `callCmdDispatcher(m_pCmdDispatcher, warehouse, 0x4E)`
- Sets `[warehouse+0x12] = 0xC` before dispatch
- Does not write `warehouse+0x5E`

### cmd 0x4F — Release

- Dispatcher: `FUN_101c69b0` (RVA `0x1c69b0`, abs `0x101c69b0`)
- Clears `warehouse+0x5E` to NULL at instruction `0x101c6a06`: `MOV dword ptr [EAX+0x5E], 0x0`
- Called by program-writer destructor chain: `FUN_101c0fd0 → FUN_101c0ca0 → FUN_101c69b0`

---

## 6. Specialized cmd 0x0F dispatcher: `FUN_101d0950`

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

### CLI wrapper: `callCmd0FDispatcher`

At `src/tsdev_lib.cpp:302-324`. SEH-protected inline-asm `__thiscall` call to `FUN_101d0950`.

Dependencies (resolved function pointers):
| Pointer | orlib20 function | RVA | Resolution |
|---------|-----------------|-----|------------|
| `m_pCmd0FDispatcher` | `FUN_101d0950` | `0x1D0950` | `sigMatchRva` of patch site `FUN_101d0950_thread_check_INT3` (`src/tsdev_lib.cpp:3059-3068`) |
| `m_pBuildTypeArray` | `FUN_101d43a0` | `0x1D43A0` | `resolveBuildTypeArray()` (`src/orlib_resolver.cpp:732-738`) |
| `m_pCelProgWriterCtor` | `FUN_1000dd30` | `0x0000DD30` | `resolveCelProgramWriterCtor()` (`src/orlib_resolver.cpp:740-746`) |

Call pattern (from `refreshCache()` at `src/tsdev_lib.cpp:2144-2160`):
```cpp
int typeArray[16] = {};
callBuildTypeArray(m_pBuildTypeArray, category, typeArray);
unsigned char writerBuf[0x280] = {};
callCelProgramWriterCtor(m_pCelProgWriterCtor, writerBuf);
callCmd0FDispatcher(m_pCmd0FDispatcher, warehouse, typeArray, progId, writerBuf);
// warehouse+0x5E is now set (on success) or cleared (on failure)
```

---

## 7. orlib20's own cmd 0x3B dispatcher: `FUN_101c51b0`

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

## 8. `_VerifyModule_New@20`

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

## 9. `FUN_101a84e0` (post-verify phase)

RVA `0x1a84e0`, abs `0x101a84e0`. `__fastcall`: ECX/EDX = first two params, first stack arg = deletion flag.

- Not exported, not in signature catalog
- Called by `CompileText` after the main compile+cleanup (if `DAT_1023aac8 != 0`)
- Calls `FUN_101d0950` (LoadProgram) again with its own prog_writer
- Dispatches: `0x35 → 0x32 → 0x3B → 0x57 → 0x47 → 0x58 → 0x4F` (from UI trace, line 84)
- Also clears `+0x5E` via the same `FUN_101c0fd0 → FUN_101c0ca0 → FUN_101c69b0` chain before returning

**Note (§17)**: A re-trace did not reproduce this — `FUN_101a84e0` was not called. The 0x3B dispatch path in the §17 trace was via `FUN_101c51b0` (RVA `0x1c51b0`) instead. The §9/§10 trace may reflect a different UI action or session condition.

---

## 10. UI trace facts

Source: `tmp/frida_ui_verify_trace.log` (94 lines, Frida trace of TSDev.exe UI verify of a newly inserted item).

| Line | Event |
|------|-------|
| 22 | `FUN_004D9480` ENTER (UI verify wrapper) |
| 23 | `FUN_004D91E0` ENTER (cat=1, sub=0, progId=78) |
| 24-29 | `FUN_004D91E0` dispatches: `0x32 0x34 0x33 0x4F 0x35 0x38` |
| 30 | `_VerifyModule_New` ENTER (cat=1, progId=0x4E, flag=1) |
| 31-73 | Commands dispatched inside `_VerifyModule_New`/`CompileText`: `0x3A 0x4E 0x4F 0x32 0x34 0x33 0x4F 0x35 0x57 0x4D 0x4F 0x53 0x45 0x32 0x34 0x36 0x48 0x4F 0x34 0x32 0x33 0x4F 0x45 0x45 0x35 0x57 0x57 0x47 0x58 0x4F 0x65 0x66 0x34 0x4F 0x35 0x4F 0x4D 0x4F 0x32 0x34 0x33 0x4F 0x4F` |
| 74 | `_VerifyModule_New` EXIT (ret=1) |
| 75 | `FUN_004D91E0` EXIT |
| 76 | `FUN_004D9480` EXIT |
| 81 | `FUN_101a84e0` ENTER |
| 84 | cmd `0x3B` dispatched (**only occurrence in trace**) |
| 89 | `FUN_101a84e0` EXIT |

**Key facts**:
- cmd `0x3B` appears exactly once (line 84), inside `FUN_101a84e0` (lines 81-89)
- cmd `0x3B` does NOT appear inside `FUN_004D91E0` (lines 23-75) or `FUN_004D9480` (lines 22-76)
- `FUN_004D91E0` dispatches 6 commands before `_VerifyModule_New`: `0x32, 0x34, 0x33, 0x4F, 0x35, 0x38`

**Note (§17)**: A re-trace did not reproduce the `FUN_101a84e0` path — `FUN_101a84e0` was not called. The 0x3B dispatch path in the §17 trace was via `FUN_101c51b0` (RVA `0x1c51b0`) instead. The §10 trace may reflect a different UI action or session condition.

---

## 11. SafeArray format (cmd 0x3B result)

Proven by dynamic test (this session) + Ghidra `FUN_101c1ed0`:

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

## 12. Component types

From `src/tsdev_lib.h:50-56`:

| Type | Enum value | Category (low 8 bits) | Sub-type (high bits) |
|------|-----------|----------------------|---------------------|
| Strategy | 0 | 0 | 0 |
| Indicator | 2 | 2 | 0 |
| Function | 4 | 4 | 0 |
| TradingApp | 5122 (0x1402) | 2 | 0x14 |
| ShowMe | 1282 (0x0502) | 2 | 0x05 |

`getCategory(type) = type & 0xFF`, `getSubType(type) = type >> 8` (`src/tsdev_lib.h:63-68`).

Three types unconfirmed: ActivityBar, PaintBar, ProbabilityMap (`AGENTS.md`).

---

## 13. CLI operations

From `AGENTS.md` and `src/tsdev_cli.cpp`:

| Operation | CLI subcommand | Implementation |
|-----------|---------------|----------------|
| Insert | `insert <type> <name> <source_file>` | `doInsert` |
| Delete | `delete <type> <name>` | `doDelete` |
| Modify | `modify <type> <name> <source_file>` | `doModify` |
| Exists | `exists <type> <name> [--case-insensitive]` | `doExists` |
| Get source | `get-source <type> <name> [--output <file>]` | `doGetSource` |
| Upsert | `upsert <type> <name> <source_file>` | `doUpsert` |
| Dependents | `dependents <type> <name>` | `doDependents` |
| Batch | `batch-<op> <manifest>` / `batch-<op>-dir <type> <dir>` | `runBatchAndEmit` |
| Serve | `serve [--serve-pipe-file <path>]` | `doServe` |

All direct CLI subcommands require elevated administrator process. Non-elevated returns exit code 11 (`AGENTS.md`).

---

## 14. Dynamic test results (this session)

All tests run from elevated `tsdev_cli.exe` built from current source.

### Test 1: Generic cmd 0x0F dispatch (REVERTED)

- Change: inserted `callCmdDispatcher(m_pCmdDispatcher, warehouse, 0x0F)` with `+0x1E=0x2000` before cmd 0x3B
- Result: status `0x80070057` (`E_INVALIDARG`), `warehouse+0x5E = NULL`, crash + `TSCrashReport.exe`
- Conclusion: generic dispatch of 0x0F is invalid; specialized `FUN_101d0950` is required
- Source change reverted; NULL guard added before cmd 0x3B

### Test 2: cmd 0x0F via `callCmd0FDispatcher` → cmd 0x3B

- Change: `enumerateCallersByProgId` now uses `callCmd0FDispatcher` with real ProgId instead of `callCmdDispatcher(0x0D)`
- `doDependents` now calls `enumerateCallersByProgId(type, er.progId)` directly instead of `enumerateCallers(type, name)` (bypassing `findExistPos`)
- Item: "Returns default Down color", function, ProgId=517
- Result: cmd 0x0F succeeded (`rootRecord=0CAB677C`), cmd 0x3B succeeded (status=0), SafeArray returned (`saPtr=0D215EF8`)
- SafeArray: `cDims=1, lb=0, ub=3, elemCount=4, cbElements=1`, 4 bytes all zeros, count prefix=0
- **Zero callers returned**
- No crash, no `TSCrashReport.exe`

### Test 3: Newly inserted item

- Inserted "Cmd0FTest" (function, ProgId=1582) with valid EasyLanguage source
- Immediately queried dependents
- Result: same as Test 2 — cmd 0x0F succeeded, cmd 0x3B succeeded, SafeArray empty (count=0)
- **Zero callers returned** (expected — nothing calls Cmd0FTest)
- Item deleted after test

### Test 4: Crash monitoring

- After each test: `tasklist | findstr /I "TSCrashReport WerFault"` — no crash dialogs found (Tests 2-3)
- Test 1 crash: `TSCrashReport.exe` (PID 32536) appeared as child of test process; terminated with explicit PID

---

## 15. Current source state

### `enumerateCallersByProgId` (`src/tsdev_lib.cpp:2385-2594`)

Current flow:
1. Get warehouse via `callGetWarehouse`
2. Call `refreshCache()`
3. Build type array via `callBuildTypeArray(m_pBuildTypeArray, category, typeArray)`
4. Construct prog_writer via `callCelProgramWriterCtor(m_pCelProgWriterCtor, writerBuf)`
5. Dispatch cmd 0x0F via `callCmd0FDispatcher(m_pCmd0FDispatcher, warehouse, typeArray, progId, writerBuf)`
6. Read `warehouse+0x5E`; abort if NULL
7. Clear `warehouse+0x26`
8. Dispatch cmd 0x3B via `callCmdDispatcher(m_pCmdDispatcher, warehouse, 0x3B)`
9. Check `warehouse+0x4E == 0`
10. Read SafeArray from `warehouse+0x26`
11. Copy SafeArray via `callersCopySafeArray`
12. Parse entries (offset 4, stride `8 + nameLength + 0x16`)

### `doDependents` (`src/tsdev_cli.cpp:1819-1862`)

Current flow:
1. `enumerateRecords(type, name)` — find item, get ProgId + stored name
2. `enumerateCallersByProgId(type, er.progId)` — get callers
3. Emit JSON with `dependents` array

---

## 16. Open facts (unknowns)

These are proven facts about what is **not** known:

1. **Why the CLI's 0x0F → 0x3B path returns zero callers while the UI's path may differ**: ~~The CLI dispatches 0x3B directly via `FUN_101cf5b0(0x3B)` after a standalone `0x0F` load (KB §14, §15); the UI dispatches 0x3B through `FUN_101c51b0`, which uses a cached IProgram and writes it to `warehouse+0x5E` (KB §17). Whether the difference in dispatch path, IProgram provenance, or additional `FUN_101c51b0` setup (re-entrance guard at `+0x1C9`, `CSingleLock` at `+0x218`, and cache management at `+0x10`; see KB §7) affects caller-table population is not proven.~~ **RESOLVED (KB §18):** The UI's `FUN_101c51b0` path also returns an empty SafeArray (count=0) on the function's IProgram. The dispatch-path difference is NOT the cause. Both the CLI and UI paths produce the same empty result.

2. **What cmds 0x35 and 0x32 do at the worker level**: Their worker handlers were not decompiled in this session. They are part of the standard compile/verify command pattern (e.g., before `_VerifyModule_New` in `FUN_004D91E0`, KB §10) and appear throughout the §17 trace, but they are **not** the immediate predecessors to 0x3B (KB §17). Whether they affect the IProgram's caller tables is not determined; this is no longer treated as relevant to why the CLI 0x3B path returns zero callers.

3. **Whether `FUN_004D91E0`'s pre-verify commands populate the dependency DB**: The UI trace shows `FUN_004D91E0` dispatches `0x32, 0x34, 0x33, 0x4F, 0x35, 0x38` before `_VerifyModule_New`. Whether these populate the DB that cmd 0x3B reads is not proven.

4. **`FUN_101e1560` (SafeArray parser) implementation**: Not decompiled in this session. The CLI's parsing format (offset 4, stride `8 + nameLength + 0x16`) is from prior documentation, not verified against `FUN_101e1560` in this session.

5. **Whether the empty SafeArray is a parsing issue**: The SafeArray has `elemCount=4, cbElements=1` (4 bytes). The CLI interprets the first 4 bytes as a count prefix. Whether orlib20's `FUN_101c51b0` interprets it the same way (via `FUN_101c1ed0` element count + `FUN_101e1560` parser) is not verified in this session.

6. **Why a UI-inserted function does not persist while a UI-inserted indicator does**: In the 2026-08-29 re-trace, UI-inserting function `E2eFnU_202608291059` (ProgId=82) reported `ok=True` (verify succeeded, no errors), but `tsdev_cli exists Function E2eFnU_202608291059 --case-insensitive` immediately after returned `exists=0`. The indicator `E2eIndU_202608291059` UI-inserted in the same session DID persist (`exists=1, progId=2994`). The same phenomenon was observed in the prior trace (E2eFnCaller "already gone" at cleanup). The reason the UI-inserted function is not persisted is NOT known. This is an open fact, not an explanation. (Note: CLI-inserted functions persist reliably.)

---

## 17. UI callers-trace evidence (TVS)

Source: `tmp/frida_ui_callers_trace.log` (1416 lines, Frida trace of TSDev.exe UI insertion + verify). Trace log captured with hooks on `FUN_101cf5b0` (generic dispatcher), `FUN_101d0950` (0x0F specialized dispatcher), `FUN_101a84e0` (post-verify phase), and `FUN_101c51b0` (orlib20's own 0x3B dispatcher).

### Setup

- Two items inserted via the TSDev UI (`TSDevUI.insert_item`): a function `TvsTraceFnUi` (`TvsTraceFnUi = 1;`) and an indicator `TvsTraceIndUi` (`Plot1(TvsTraceFnUi, "Trace");`) that calls the function.
- A third item `TvsTraceFn` (function, CLI-inserted) was also re-verified via the UI (open + dirty + F3) to test whether re-verify triggers 0x3B.
- Frida JS: `tmp/frida_ui_callers_trace.js`; Python driver: `tmp/frida_ui_callers_trace.py`.

### Key finding: 0x3B dispatch path

**cmd 0x3B was dispatched via `FUN_101c51b0` (RVA `0x1c51b0`), NOT via `FUN_101a84e0`.**

- `FUN_101a84e0` (RVA `0x1a84e0`) was hooked but **never called** during UI insertion or re-verify. The KB §10 claim that 0x3B appears "inside FUN_101a84e0" is not reproduced in this trace. `FUN_101a84e0` may only run under conditions not met here (e.g. a different UI action or a first-ever verify of a UI-created item in a prior TSDev version/session).
- `FUN_101c51b0` fired exactly once (trace event `[c51b0 #604]`), during the indicator UI-insert compile phase, and dispatched 0x3B via `FUN_101cf5b0(0x3B)`.

### Exact cmd sequence around 0x3B (indicator UI-insert)

Verbatim from `tmp/frida_ui_callers_trace.log` lines 1221-1224 (the 0x3B dispatch):

```
[c51b0 #604] ENTER ctx=0x8fc838 wh=0xbc3d018 cachedIProg=0x13e463c4
[generic #605] ENTER cmd=0x3b wh=0xbc3d018 wh+04(cmd)=0x32 wh+12(wht)=0x4f wh+16(pid)=0xbac wh+1E(load)=00200000000000006093cf1300000000 wh+26(sa)=0x13cf9360 wh+4E(hr)=0x0 wh+5E(iprog)=0x13e463c4
[generic #605] EXIT cmd=0x3b ret=0x1 wh+4E(hr)=0x0 wh+26(sa)=0x13cf9420 wh+5E(iprog)=0x13e463c4
[c51b0 #604] EXIT ret=0x1
```

Immediate cmd sequence before 0x3B (events #597-#605):

| Event | Cmd | wh+5E (IProgram) | wh+26 (SafeArray) | wh+4E (HRESULT) |
|-------|-----|-------------------|---------------------|-------------------|
| #597 | 0x0F-spec (LoadProgram, progId=0xbac) | 0x13e4601c | 0x1a0f5608 | 0x0 |
| #598 | 0x0F-spec (LoadProgram, progId=0x4f, outParam=0x8fc838) | 0x13e463c4 | 0x1a742d00 | 0x0 |
| #599 | 0x34 | 0x13e463c4 | 0x1a742d00 | 0x0 |
| #600 | 0x32 | 0x13e463c4 | 0x1a742d00 | 0x0 |
| #601 | 0x3a | 0x13e4601c | 0x13cf9360 | 0x0 |
| #602 | 0x34 | 0x13e4601c | 0x13cf9360 | 0x0 |
| #603 | 0x32 | 0x13e4601c | 0x13cf9360 | 0x0 |
| #604 | **c51b0 ENTER** (ctx=0x8fc838, cachedIProg=0x13e463c4) | — | — | — |
| #605 | **0x3B** | 0x13e463c4 | 0x13cf9360→0x13cf9420 | 0x0 |

Cmds after 0x3B: `0x4c, 0x4f, 0x4b, 0x58, 0x4f, 0x32, 0x34, 0x33, 0x4f, 0x4f, 0x35, 0x38`

### Where 0x35 and 0x32 appear

0x35 and 0x32 appear throughout the compile sequence in repeating groups of `(0x32, 0x34, 0x33, 0x4F, 0x35)` — the standard compile/verify cmd pattern also seen in KB §10. They are **not** the immediate predecessors to 0x3B. The immediate predecessors are `0x34, 0x32, 0x3a, 0x34, 0x32`, followed by `FUN_101c51b0` which dispatches 0x3B.

Both 0x35 and 0x32 are dispatched via the generic dispatcher `FUN_101cf5b0` with **only a cmdId** (DWORD at `[esp+4]`), no extra parameters.

### FUN_101c51b0 mechanism (confirmed by trace)

1. The compile code loads an IProgram via `FUN_101d0950` (0x0F-spec, event #598) into a prog_writer buffer at `outParam=0x8fc838`. The IProgram (0x13e463c4) is stored at `prog_writer+0x10`.
2. `FUN_101c51b0` is called with `ctx=0x8fc838` (the prog_writer). It reads `cachedIProg = [ctx+0x10] = 0x13e463c4` (cache is pre-populated; no FindProgram/cmd 0x0D needed).
3. `FUN_101c51b0` writes the cached IProgram to `warehouse+0x5E` (visible at #605: `wh+5E=0x13e463c4`).
4. `FUN_101c51b0` dispatches 0x3B via `FUN_101cf5b0(0x3B)` (event #605).
5. 0x3B result: `wh+4E=0x0` (success), `wh+26` changed from `0x13cf9360` to `0x13cf9420` (SafeArray modified — non-zero pointer).

### What did NOT trigger 0x3B

- **Re-verifying a CLI-inserted item** (v3 trace, `TvsTraceInd` indicator): full compile fired (0x32, 0x34, 0x33, 0x4F, 0x35, 0x0F-spec, 0x3A, 0x4E, ...) but **no 0x3B, no c51b0, no a84e0**.
- **Re-verifying a UI-inserted function** (`TvsTraceFnUi`): full compile fired but **no 0x3B, no c51b0, no a84e0**.
- **UI-inserting a function** (`TvsTraceFnUi`): full compile fired but **no 0x3B, no c51b0**.

0x3B fired **only** during the **indicator UI-insert** — i.e. when the compiled item (indicator) references another item (function) as a dependency.

### Conclusion

1. **0x35 and 0x32 are NOT the missing cmds before 0x3B.** They are part of the standard compile cmd sequence and appear throughout, but they are not immediate predecessors to 0x3B. They take only a cmdId (no extra params) via `FUN_101cf5b0`.

2. **The UI and CLI use different 0x3B dispatch paths.** In the UI trace, `FUN_101c51b0`:
   - Receives a context object (prog_writer) containing a cached IProgram loaded via `FUN_101d0950` (0x0F-spec, event #598)
   - Writes the cached IProgram to `warehouse+0x5E`
   - Dispatches 0x3B via `FUN_101cf5b0(0x3B)`
   - Parses the SafeArray result (per KB §7)

   The CLI dispatches 0x3B directly via `FUN_101cf5b0(0x3B)` after loading the IProgram via `0x0F` (KB §15). Whether the difference in dispatch path, IProgram provenance, or additional `FUN_101c51b0` setup affects the caller-table population is not proven.

3. **`FUN_101a84e0` is NOT the 0x3B dispatch path** in this scenario. The KB §10 trace's claim that 0x3B appears inside `FUN_101a84e0` is not reproduced. The actual 0x3B dispatch is via `FUN_101c51b0`.

4. **In this trace, 0x3B fired only during the indicator UI-insert.** It did not fire during: (a) function UI-insert, (b) function UI re-verify, or (c) CLI-inserted indicator re-verify. Whether 0x3B fires only for items with dependencies is not proven; the observed correlation is consistent with that hypothesis.

5. **The 0x3B SafeArray pointer was non-zero** (0x13cf9420, changed from 0x13cf9360), confirming 0x3B produced a SafeArray. Whether it contained non-zero caller entries could not be determined from the trace alone (the SafeArray content was not dumped). The 0x3B was dispatched on IProgram `0x13e463c4` (loaded by event #598, a `0x0F-spec` dispatch with `progId=0x4f`); the identity of that IProgram is unknown from this trace. It is not the indicator's IProgram (`0x13e4601c`, loaded by event #597 with `progId=0xbac`), which `FUN_101c51b0` did not use.

### Next step

**SUPERSEDED by §18.** §18 captured 0x3B on the function's IProgram via `FUN_101c51b0` and the result was EMPTY. TSDev.exe has no "Find Dependents" feature for regular functions/indicators (see §18 investigation). The next step is now in §18 / WORKING_MEMORY.md: investigate whether the WHServer.exe dependency DB is populated at all.

---

## 18. UI callers-trace: provably non-empty result NOT obtained — 0x3B on function's IProgram returns EMPTY

Source: `tmp/frida_ui_callers_nonempty.log` (113 lines, Frida trace of TSDev.exe UI insertion + verify). Trace log captured with hooks on `FUN_101cf5b0` (generic dispatcher), `FUN_101d0950` (0x0F specialized dispatcher), and `FUN_101c51b0` (orlib20's own 0x3B dispatcher). SafeArray contents were dumped for the first time.

### Setup

- Two items inserted via the TSDev UI (`ui_insert_item`):
  - Function `E2eFnCaller` (`E2eFnCaller = 1;`), ProgId=80 (0x50), IProgram=0x179e160c
  - Indicator `E2eIndCallerFn` (`Plot1(E2eFnCaller, "E2e");`), ProgId=2990 (0xbae), IProgram=0x179e1d14
- The indicator calls the function, so the function's caller = the indicator.
- Frida JS: `tmp/frida_ui_callers_nonempty.js`; Python driver: `tmp/frida_ui_callers_nonempty.py`.
- TSDev.exe was started normally (not Frida-spawned) and Frida attached after UI init.

### Key finding: 0x3B on the FUNCTION's IProgram returns EMPTY SafeArray

**cmd 0x3B was dispatched via `FUN_101c51b0` on the function's IProgram (0x179e160c, ProgId=80), and the SafeArray result is EMPTY (count=0, 4 bytes all zeros).**

This is the same result as the CLI's 0x0F → 0x3B path (KB §14, §15). The FUN_101c51b0 dispatch path does NOT populate callers.

### Exact cmd sequence around 0x3B (indicator UI-insert)

Verbatim from `tmp/frida_ui_callers_nonempty.log` lines 57-66:

```
[0x0F-spec #106] ENTER wh=0xa86c1c0 typeArray=0x8fcd2c progId=80 (0x50) outParam=0x8fc838 ...
[0x0F-spec #106] EXIT ret=0x1 progId=80 wh+4E(hr)=0x0 wh+26(sa)=0x125bc9e0 wh+5E(iprog)=0x179e160c outParam=0x8fc838 [outParam+0x10]=0x179e160c
[c51b0 #112] ENTER ctx=0x8fc838 wh=0xa86c1c0 cachedIProg=0x179e160c whType=0x1 name="" (progId map: {"80":"0x179e160c","2990":"0x179e1d14"})
[generic #113] ENTER cmd=0x3b wh=0xa86c1c0 wh+04(cmd)=0x32 wh+12(wht)=0x50 wh+16(pid)=0xbae ... wh+5E(iprog)=0x179e160c
[generic #113] EXIT cmd=0x3b ret=0x1 wh+4E(hr)=0x0 wh+26(sa)=0xabce608 wh+5E(iprog)=0x179e160c
[safearray cmd0x3B #113] cDims=1 fFeatures=0x80 cbElements=1 cLocks=0 pvData=0x189bed80 cElements=4 lLbound=0 totalBytes=4
[safearray cmd0x3B #113] first 4 bytes: 00 00 00 00
[safearray cmd0x3B #113] countPrefix=0
[safearray cmd0x3B #113] parsed 0 entries
[c51b0 #112] EXIT ret=0x1 wh+4E(hr)=0x570079 wh+26(sa)=0xd19c1761
```

### IProgram identity confirmed

- Event #106: `0x0F-spec` loads ProgId=80 (the function E2eFnCaller) into outParam=0x8fc838. The IProgram (0x179e160c) is stored at `outParam+0x10`.
- Event #112: `FUN_101c51b0` is called with `ctx=0x8fc838` (the prog_writer from event #106). It reads `cachedIProg = [ctx+0x10] = 0x179e160c` — this IS the function's IProgram.
- Event #113: cmd 0x3B is dispatched with `wh+5E=0x179e160c` (the function's IProgram).
- The progId map confirms: `{"80":"0x179e160c","2990":"0x179e1d14"}` — ProgId 80 = function, ProgId 2990 = indicator.

### SafeArray dump (first time captured)

The SafeArray at `warehouse+0x26` after 0x3B:
- `cDims=1, fFeatures=0x80 (FADF_HAVEVARTYPE), cbElements=1, cLocks=0`
- `pvData=0x189bed80, cElements=4, lLbound=0, totalBytes=4`
- First 4 bytes: `00 00 00 00`
- Count prefix (little-endian int32): **0**
- Entries parsed: **0**

This is identical to the CLI's result (KB §14 Test 2): `elemCount=4, cbElements=1`, 4 bytes all zeros, count prefix=0.

### c51b0 context fields

The `FUN_101c51b0` context (ctx=0x8fc838) shows:
- `whType=0x1` (not 0x4 as expected for Function; this may be a prog_writer field, not the warehouse whType)
- `name=""` (empty string at ctx+0x18; the name pointer may not be set in this code path, or the name is stored differently)

### What did NOT trigger 0x3B

- **Function UI-insert**: no 0x3B, no c51b0 (same as §17)
- **Function re-verify (open+F3)**: no 0x3B, no c51b0 (same as §17)
- **Find Dependents**: TSDev.exe has NO "Find Dependents" feature for regular functions/indicators. See "Find Dependents investigation" below.
- **Find Dependents via Tools/Edit menu**: no such menu item exists.

0x3B fired **only** during the **indicator UI-insert** — same as §17.

### Find Dependents investigation (adversarial-review fix)

A genuine effort was made to locate TSDev.exe's "Find Dependents" feature. It does NOT exist for regular EasyLanguage functions/indicators:

1. **Menu enumeration**: `win32gui.GetMenu` on the main window returns `hmenu=0x0` (TSDev uses a BCGPSoft owner-drawn menu bar, `BCGPToolBar` class "Menu Bar", not a standard Win32 HMENU). pywinauto `menu()` returns None. The UIA backend found only the "System" MenuBar (the system menu), not the app menu. Keyboard Alt+<key> walks produced no `#32768` popup windows (BCGP menus are custom-drawn). Artifacts: `tmp/enumerate_menus.log`, `tmp/enumerate_menus_v2.log`, `tmp/enumerate_menus_v3.log`.

2. **Binary string search**: `strings` on TSDev.exe (narrow + wide) found NO "Find Dependents", "Find References", "Where Used", or "Used By" menu text. The strings "Dependencies" and "Callers" exist but appear in a sequence of tree-view node names: `TradingApp, UserFunction, SystemFunction, Icon, Form, Dependencies, Strings, Callers, Image` — these are **TradingApp project resource tree nodes**, not a function/indicator feature. Artifacts: `tmp/tsdev_strings.txt`, `tmp/tsdev_strings_wide.txt`.

3. **Outline pane inspection**: Opening a function (built-in `Average` and user `E2eFn_202608291052`) in the editor and dumping the Outline pane `SysTreeView32` showed only a `Document` node (with `using`, `inputs`, `Price`, `Length` children). No `Callers` or `Dependencies` node. Artifact: `tmp/inspect_outline_user.log`.

4. **Resource View pane**: Shows `Resource View > <name> > Resources > Forms/Strings/Images` for user functions — no `Callers`/`Dependencies` node.

5. **orlib20.dll strings**: Contains `GetDependencies`, `_GetStaticDependencyList@12`, `_GetDynamicDependencyList@12`, `_DeleteDependencyList@4` — dependency APIs, but no UI text.

**Conclusion**: TSDev.exe does not expose a "Find Dependents" UI action for regular functions/indicators. The 0x3B evidence is from the **compile path only** (indicator UI-insert compile dispatches 0x3B via `FUN_101c51b0`).

### Re-trace with DEFECT-5-fixed JS (2026-08-29)

A second Frida trace (`tmp/frida_find_dependents_trace2.log`) re-captured 0x3B with the fixed JS (c51b0 onLeave no longer reads garbage wh fields). Setup: UI-inserted function `E2eFnU_202608291059` (ProgId=82) + indicator `E2eIndU_202608291059` (ProgId=2994) referencing it, unique names verified free via `tsdev_cli exists` before insert.

Verbatim evidence (0x3B on the function's IProgram, ProgId=82, IProgram=0x12b52a64):
```
[c51b0 #133] ENTER ctx=0x8fc838 wh=0xa7634f0 cachedIProg=0x12b52a64 whType=0x1 name="" (progId map: {"82":"0x12b52a64","2994":"0x1292ea8c"})
[generic #134] ENTER cmd=0x3b wh=0xa7634f0 wh+04(cmd)=0x32 wh+12(wht)=0x52 wh+16(pid)=0xbb2 ... wh+5E(iprog)=0x12b52a64
[generic #134] EXIT cmd=0x3b ret=0x1 wh+4E(hr)=0x0 wh+26(sa)=0xaabadb8 wh+5E(iprog)=0x12b52a64
[safearray cmd0x3B #134] cDims=1 fFeatures=0x80 cbElements=1 cLocks=0 pvData=0x13f1d320 cElements=4 lLbound=0 totalBytes=4
[safearray cmd0x3B #134] first 4 bytes: 00 00 00 00
[safearray cmd0x3B #134] countPrefix=0
[safearray cmd0x3B #134] parsed 0 entries
[c51b0 #133] EXIT ret=0x1 (wh/SafeArray result captured by generic-dispatcher hook)
```

The c51b0 onLeave now logs `(wh/SafeArray result captured by generic-dispatcher hook)` instead of the garbage `wh+4E(hr)=0x570079 wh+26(sa)=0xd19c1761` seen in the prior trace. The 0x3B SafeArray is EMPTY (count=0, 4 bytes all zeros) — confirms the prior finding with the fixed JS.

### Persistence verification (adversarial-review fix)

`tsdev_cli exists --case-insensitive` was run before and after each insert:

- **E2eFnU_202608291059** (function, UI-insert): pre-insert `exists=0`; post-insert `exists=0` (NOT persisted — see open fact KB §16 item 6); at cleanup `exists=0`.
- **E2eIndU_202608291059** (indicator, UI-insert): pre-insert `exists=0`; post-insert `exists=1, stored_name=E2eIndU_202608291059, progId=2994` (persisted); deleted → post-delete `exists=0` (confirmed removed).
- **E2eFn_202608291052** (function, CLI-insert): pre-insert `exists=0`; post-insert `exists=1, progId=1588` (persisted); deleted → post-delete `exists=0` (confirmed removed).
- **E2eIndCaller_202608291052** (indicator, CLI-insert): pre-insert `exists=0`; post-insert `exists=1, progId=2992` (persisted); deleted → post-delete `exists=0` (confirmed removed).

CLI-inserted items persist reliably. The UI-inserted function did NOT persist (open fact, not speculation).

### Popup/crash observations

- **No crash dialogs** (TSCrashReport.exe / WerFault.exe) were observed at any point during the trace.
- **No error/warning popups** were observed during UI insertion, re-verify, or Find Dependents attempts.
- All crash checks returned OK after every action.
- Cleanup via elevated CLI: E2eIndCallerFn was deleted (confirmed `exists=0` after). E2eFnCaller was NOT found by `tsdev_cli exists` at cleanup time (`exists=0`). The reason it was absent is NOT known — recorded as an open fact (KB §16 item 6), not explained by speculation.

### Verdict

**The FUN_101c51b0 path does NOT populate callers.** The 0x3B result on the function's IProgram (ProgId=80, IProgram=0x179e160c) is EMPTY (count=0, 4 bytes all zeros), even though the indicator E2eIndCallerFn (ProgId=2990) calls the function E2eFnCaller. This is the same empty result as the CLI's 0x0F → 0x3B path.

The dispatch-path difference (FUN_101c51b0 vs direct FUN_101cf5b0) is NOT the cause of the empty caller list. Both paths produce the same empty SafeArray.

**Caveat (RESOLVED 2026-08-29, TVS below):** All 0x3B traces in §18 were on UI-inserted functions that did NOT persist (`exists=0` after UI insert — see KB §16 item 6), so the empty result could have been a non-persistence artifact. This is now RULED OUT — see "Persistence-ruled-out TVS" below.

### Persistence-ruled-out TVS (2026-08-29)

Tested `dependents` on items persisted via the CLI (which persist reliably — KB §16 item 6). Function `E2eFnDep_113051` (ProgId=1591, source `E2eFnDep_113051 = 1;`) and indicator `E2eIndDep_113051` (ProgId=2995, source `Variables: v(0); v = E2eFnDep_113051; Plot1(v, "Val");`) were both CLI-inserted with EXIT=0 and confirmed `exists=1`. The indicator compiled successfully while referencing the function, so a real caller relationship existed at compile time. `tsdev_cli dependents function E2eFnDep_113051` (and `--json`) still returned `dependents_count=0` / `"dependents":[]`; the internal trace shows cmd 0x3B returned a 4-byte all-zero SafeArray (`countField=0`), identical to all prior traces. The CLI's `doDependents` faithfully dispatches 0x0F→0x3B via `enumerateCallersByProgId` with no short-circuit (confirmed by adversarial source review). No crashes/popups. Both items cleaned up (exists=0, .ell/.xml removed). Full log: `tmp/tvs_dependents_persisted.log`.

**Conclusion:** The empty-caller result is NOT caused by non-persistence of the queried item. Even for persisted items with a genuine, compiler-resolved caller relationship, cmd 0x3B returns an empty SafeArray. The cause is downstream of persistence — i.e. the dependency/caller table that 0x3B reads is not populated by the CLI insert/compile path for these items.

### Implication

The empty caller list is NOT caused by:
1. The dispatch path (FUN_101c51b0 vs direct FUN_101cf5b0) — both produce empty results
2. The IProgram provenance (0x0F LoadProgram vs 0x0D FindProgram) — the UI uses 0x0F and still gets empty
3. Missing predecessor commands (0x35, 0x32) — these appear in the compile sequence but are not immediate predecessors to 0x3B
4. A missing "Find Dependents" UI trigger — TSDev.exe has no such feature for functions/indicators (see "Find Dependents investigation" above)
5. Non-persistence of the queried item — RULED OUT by the persistence-ruled-out TVS above (both items persisted with exists=1 and a compiler-resolved caller relationship; 0x3B still returned empty)

The empty caller list is likely caused by:
- The dependency database (in WHServer.exe / COM server) not being populated with the caller relationship at the time 0x3B is dispatched — the CLI insert/compile path does not appear to write the caller relationship into the table that 0x3B reads
- Or the IProgram::Callers vtable method (vtable+0x38) itself returning an empty SafeArray for the given IProgram, regardless of dispatch path
- Or the IProgram loaded via 0x0F not having its caller table populated despite being "fully-loaded"

The next investigation should focus on the **higher-level** question of whether the WHServer.exe COM server's dependency database is populated at all for these items (a state question), e.g. query `GetStaticDependencyList`/`GetDynamicDependencyList` via orlib20 (KB §18 strings) or inspect the warehouse dependency tables directly. Do NOT jump to vtable+0x38 introspection — AGENTS.md's "don't dig deeper" rule applies unless higher-level causes are ruled out first.

---

## 19. TS Desktop cross-process notification mechanism

1. **NotifyLibChanged (orlib20 ordinal 57) is per-process, NOT cross-process.** orlib20 `FUN_101526f0` (ordinal 57, `_NotifyLibChanged@8`) packs `lParam = (category << 16) | progId` and calls `tslib::EventsViaHandles::PosMessageToClients(&PTR_DAT_10231050, 0x40A, 0, lParam)`, which iterates the HWND array at `PTR_DAT_10231050` and calls `PostMessageW(hwnd, 0x40A, 0, lParam)` per registered HWND. `PTR_DAT_10231050` is at `0x10231050`, inside orlib20's PRIVATE `.data` section (`0x1022e000-0x10240987`), NOT in the shared `SHARE_WO` (`0x10243000-0x104889ff`) or `SHARE_MM` (`0x10489000-0x104891ff`) sections. Therefore each process loading orlib20 has its own `EventsViaHandles` HWND array; a `0x40A` posted from one process only reaches HWNDs registered in THAT process's orlib20.

2. **The CLI's 0x40A reaches no windows.** The CLI never calls `_OrlibRegisterNotificationHandle@4` (ordinal 595, `FUN_1011c130`), so no HWNDs are registered in the CLI's orlib20 `EventsViaHandles`. The CLI does call `NotifyLibChanged` after insert/modify (`verifyModule` success, `src/tsdev_lib.cpp:2031-2033`) and after delete (`deleteEntry` success, `category != 4`, `src/tsdev_lib.cpp:2961-2963`), but the posted `0x40A` goes to an empty HWND list.

3. **TSDev.exe's 0x40A also does NOT cross to TS Desktop.** TSDev.exe's verify-success path (`FUN_00453f70` @ `0x00453f70`) calls `ShowRecalculationWarning()` then `Ordinal_57(category, progId)`, posting `0x40A` to TSDev.exe's own orlib20's registered HWNDs (TSDev's own views). It is an in-process UI refresh signal for TSDev's own MFC views, not a cross-process notification to TS Desktop.

4. **TS Desktop (orchart.exe) registers for 0x40A in its own process.** orchart.exe imports `_OrlibRegisterNotificationHandle@4` (string at `0x00781414` in orchart.exe). It registers its own window HWND with its own orlib20's `EventsViaHandles`. This registration is invisible to other processes.

5. **The actual cross-process synchronization is the shared warehouse memory + WHServer.exe COM Connection Points.** orlib20's warehouse data lives in the `SHARE_WO` section (`0x10243000-0x104889ff`, 2.38 MB, shared across all processes loading orlib20). orlib20's `_CreateEntryEX@24` (`FUN_10159610` @ `0x10159610`) makes a vtable call through `warehouse+0xAE` (the WHServer.exe COM server interface). WHServer.exe implements `IConnectionPointContainerImpl<CWHouse>` with `_IWHouseEvents` (RTTI strings at `0x0043ea4c`, `0x0043ea80` in WHServer.exe) and `IConnectionPointContainerImpl<CProgram>` with `_IProgramEvents`. TS Desktop subscribes to `_IWHouseEvents` via `IConnectionPoint::Advise`.

6. **Empirical confirmation (2026-08-29, indicator cat=2 only):** A CLI-inserted indicator (`TsDeskProbe_20260829123929`, source `Plot1(Close,"Probe");`, ProgId=2996) was immediately visible, applicable, and plotted correctly in a running TS Desktop with no restart and no manual refresh. A CLI deletion of the same indicator (while applied to a chart) was auto-detected by TS Desktop: the applied indicator was removed from the chart and disappeared from the Insert Analysis Techniques list, matching TSDev behavior. This confirms the shared-warehouse + WHServer COM path is sufficient for indicator insert+delete TS Desktop interaction with NO code change. Type coverage (strategy/tradingapp/showme/function), dependent/encrypted/batch cases are UNVERIFIED. Log: `tmp/tsdesk_probe.log`.

7. **Notification registration exports:** `_OrlibRegisterNotificationHandle@4` (ordinal 595, `FUN_1011c130` @ `0x1011c130`) calls `EventsViaHandles::RegisterHandle(&PTR_DAT_10231050, hwnd)`. `_OrlibUnregisterNotificationHandle@4` (ordinal 596, `FUN_1011c140` @ `0x1011c140`) unregisters.

8. **Delete-side additional messages:** orlib20's delete path (`FUN_10159ae0` @ `0x10159ae0`) sends `SendMessageToClients(0x40C, 1, ...)` (delete start), `0x40C, 2` (in progress), and `0x40C, 3` (delete end) around the delete orchestrator, plus `PosMessageToClients(0x40A, 0, ...)` (library changed) when `category != 4`. These `0x40C` messages are also per-process (same `EventsViaHandles`).

---

## 20. Read-only function classification

Evidence from orlib20 function/type mapping, property construction, and master-record diagnostics; no assumptions.

### Type and name mapping for category 4 / Function

| Function | orlib20 address | Output when arg = `4` | Evidence |
|----------|-----------------|----------------------|----------|
| `GetFileTypeName1` | `0x10157dc0` | `"Function (Read Only)"` | orlib20 decompilation / disassembly |
| `FUN_101583c0` | `0x101583c0` | `"FunctionReadOnly"` | orlib20 decompilation / disassembly |

### `READONLY` property construction

| Function | orlib20 address | Role | Evidence |
|----------|-----------------|------|----------|
| `FUN_10168b20` | `0x10168b20` | Builds property data containing `"READONLY" = "TRUE"` | orlib20 decompilation / disassembly |
| `FUN_10170a30` | `0x10170a30` | Builds property data containing `"READONLY" = "TRUE"` | orlib20 decompilation / disassembly |
| `FUN_101a1bd0` | `0x101a1bd0` | Builds property data containing `"READONLY" = "TRUE"` | orlib20 decompilation / disassembly |

### `SaveModule_New` flag mapping

| Fact | Evidence |
|------|----------|
| orlib20 `_SaveModule_New@20` is `FUN_10159770` at `0x10159770` | orlib20 decompilation / disassembly; `src/tsdev_lib.cpp:1572-1573` call site |
| Maps the `saveFlags` word to master-record flag bits | Ghidra decompilation of `FUN_10159770` |
| Bits 5/6 (IsProtected / IsProtectedPlus) are forced for all items saved by the production path | `src/tsdev_lib.cpp:1553-1560` TSDev-parity construction |
| Internal default password is `!p$Kn@aSus` (10 bytes in a 64-byte zeroed buffer) | `src/tsdev_lib.cpp:1557-1565` |

Included to show the production `saveSource` path sets only bits 5/6 (protection) and `0x10` (verified) — it does not set any read-only bit, so a read-only classification does not arise from the current production insert/modify path.

### Master-record offsets (fresh function)

Source: `diag_m4_raw_rec` inserted `TS_AUTO_M4_DIAG_RFUNC` (ProgId 1593) and dumped its 0x10A-byte master record.

These are the **baseline** bytes for a fresh, production-path (non-read-only) function; they are NOT read-only markers. The read-only flag's exact byte location in the deleted functions is unknown (see Evidence limitation).

| Offset | Size | Value | Interpretation |
|--------|------|-------|----------------|
| `+0x7B` | USHORT | `0x0404` | Low byte = warehouse category 4; bit 10 (`0x400`) set = internal-encryption flag (`src/tsdev_lib.cpp:1585-1589`) |
| `+0x109` | byte | `0x18` | Baseline byte for a fresh, production-path function; no proven read-only interpretation. |

### TSDev File Open `R` attribute

- TradeStation documentation for the File Open dialog states attribute code `R` applies only to EasyLanguage functions and means the function code is read-only.
- Source: `https://help.tradestation.com/10_00/eng/tsdevhelp/el_editor/file_open_eldoc.htm`.

### Evidence limitation

- The exact pre-delete master-record/XML byte(s) for `TS_AUTO_M4_FRIDA_FUNC` (ProgId 1439) and `TS_AUTO_M4_TRAV_FUNC` (ProgId 1441) were not captured before deletion. The precise byte-level flag that produced the read-only display is therefore inferred from the orlib20 string/type-mapping and property-string evidence above, not directly observed.
