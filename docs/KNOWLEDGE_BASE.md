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

1. **Why cmd 0x3B returns zero callers after cmd 0x0F**: The cmd 0x0F → cmd 0x3B path loads a fully-loaded IProgram and dispatches Callers successfully, but the SafeArray contains zero entries. The cause is not determined. The UI trace shows `FUN_101a84e0` dispatches `0x35 → 0x32` before `0x3B`; whether these commands are required to populate the caller tables is not proven.

2. **What cmds 0x35 and 0x32 do**: Their worker handlers were not decompiled in this session. The UI trace shows them dispatched before 0x3B in `FUN_101a84e0`, and before `_VerifyModule_New` in `FUN_004D91E0`. Their effect on the IProgram's caller tables is not determined.

3. **Whether `FUN_004D91E0`'s pre-verify commands populate the dependency DB**: The UI trace shows `FUN_004D91E0` dispatches `0x32, 0x34, 0x33, 0x4F, 0x35, 0x38` before `_VerifyModule_New`. Whether these populate the DB that cmd 0x3B reads is not proven.

4. **`FUN_101e1560` (SafeArray parser) implementation**: Not decompiled in this session. The CLI's parsing format (offset 4, stride `8 + nameLength + 0x16`) is from prior documentation, not verified against `FUN_101e1560` in this session.

5. **Whether the empty SafeArray is a parsing issue**: The SafeArray has `elemCount=4, cbElements=1` (4 bytes). The CLI interprets the first 4 bytes as a count prefix. Whether orlib20's `FUN_101c51b0` interprets it the same way (via `FUN_101c1ed0` element count + `FUN_101e1560` parser) is not verified in this session.
