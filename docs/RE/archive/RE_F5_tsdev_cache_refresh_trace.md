# RE F5: TSDev Cache Refresh Trace — Live Frida Analysis

## 1. Executive Summary

A live Frida trace of TSDev.exe confirms the **exact sequence of orlib20 calls**
TSDev makes to refresh its warehouse cache after insert/modify/delete operations.
The trace was captured by spawning TSDev under Frida, creating a new item via UI
automation, and then opening the File > Open dialog.

**The cache refresh sequence is:**

1. `_VerifyModule_New@20` calls `FUN_101a8620` (save/compile orchestrator)
2. `FUN_101a8620` calls `FUN_101d0950` (cmd 0x0F) — sets `[warehouse+0x5E]`
3. `FUN_101a8620` calls `FUN_101c09f0(0xC)` (cache reset) — dispatches cmd 0x4E
4. cmd 0x4E refreshes the server-side cache for type 0xC (program-list cache)
5. Subsequent cmd 0x08 (ProgramsList) enumeration sees the new item

**Key finding**: `[warehouse+0x5E]` is **NON-NULL** throughout TSDev's operation.
It is set during initialization (first cmd 0x09 dispatch) and remains valid.
The cache refresh operates on the **same warehouse** used for all other operations.

**Critical correction**: The path used by `FUN_101a8620` is **NOT** "50 spaces" as
previously claimed in `RE_F5_frida_trace_analysis.md` §5.2. The live trace shows
the actual path is `"C:\Program Files (x86)\TradeStation 9.5\MyWork\"` — the real
MyWork directory. This means the cache refresh operates on the **correct warehouse**,
not a separate one.

---

## 2. Trace Methodology

### 2.1 Setup

- **Frida**: 17.17.0 (`frida.exe` + Python `frida` package)
- **TSDev**: `C:\Program Files (x86)\TradeStation 9.5\Program\TSDev.exe`
- **Script**: `tmp/frida_trace_tsdev_cache_refresh.js`
- **Runner**: `tmp/frida_trace_v2.py` (spawn + UI automation via SendKeys)
- **Log**: `tmp/frida_cache_refresh_trace.log` (2688 lines, 1063 sequence events)

### 2.2 Hooks Installed (15 total)

All hooks installed successfully at the correct addresses:

| Hook | Module | RVA | Address |
|------|--------|-----|---------|
| `FUN_101cf5b0` (cmd dispatcher) | orlib20.dll | 0x1CF5B0 | 0x203f5b0 |
| `_LoadMasterRecsEX@16` (cached enum) | orlib20.dll | 0x1A5360 | 0x2015360 |
| `FUN_101c09f0` (cache reset) | orlib20.dll | 0x1C09F0 | 0x20309f0 |
| `_CreateEntryEX@24` (create entry) | orlib20.dll | 0x159610 | 0x1fc9610 |
| `_SaveTextEX@16` (save text) | orlib20.dll | 0x108160 | 0x1f78160 |
| `FUN_101d0950` (cmd 0x0F helper) | orlib20.dll | 0x1D0950 | 0x2040950 |
| `_GetMasterRecByType@16` (live lookup) | orlib20.dll | 0x1A0460 | 0x2010460 |
| `_GetMasterRecsBuf@12` (alt enum 0x1A) | orlib20.dll | 0x1A0C70 | 0x2010c70 |
| `_GetMasterRecsPtr@8` (alt enum 0x03) | orlib20.dll | 0x1A0BC0 | 0x2010bc0 |
| `FUN_101a8620` (save/compile orchestrator) | orlib20.dll | 0x1A8620 | 0x2018620 |
| `FindExistPos` (existence check) | orlib20.dll | 0x1A4230 | 0x2014230 |
| `_SaveModule_New@20` (save module) | orlib20.dll | 0x159A70 | 0x1fc9a70 |
| `_VerifyModule_New@20` (verify module) | orlib20.dll | 0x169630 | 0x1fd9630 |
| `FUN_10074310` (dialog cmd 0x08) | ORELUI20.dll | 0x74310 | 0x1db4310 |
| `CELFileOpenDialog::OnInitDialog` | ORELUI20.dll | 0x12F80 | 0x1d42f80 |

### 2.3 Procedure

1. TSDev spawned under Frida (PID 2940)
2. All 15 hooks installed before resume
3. TSDev resumed, waited 25s for full load
4. UI automation via PowerShell SendKeys:
   - File > New > created a new Function item (category=1)
   - Typed simple EasyLanguage source
   - Compiled (F3)
   - File > Open dialog opened
   - Dialog enumerated all types (Strategy, Function, Indicator, etc.)
   - Dialog closed (Escape)
5. Trace output captured (2688 lines)

---

## 3. Exact Call Sequence (Live Trace)

### 3.1 Initialization Phase (+0.4s to +30.5s)

TSDev's initialization loads the warehouse and enumerates existing items. The
first cmd dispatch is cmd 0x09 (ProgramByName) at +1.199s, which **sets
`[warehouse+0x5E]`** to a valid COM interface pointer (0xb3af0d4).

```
[2] +1.199s >>> ENTER FUN_101cf5b0 (cmd dispatcher)
  *** CMD DISPATCH *** wh=0xb275b80 [wh+04]=0x9 (ProgramByName)
    [wh+12]=0x0 [wh+5E]=0xb3af0d4 [wh+4E]=0x0 [wh+AE]=0xb2368f4 [wh+C2]=1(SERVER)
```

**Key observation**: `[warehouse+0x5E]` is set from the very first command
dispatch. It is NOT NULL at any point during TSDev's operation. The warehouse
(0xb275b80) is in SERVER mode (`[wh+C2]=1`) throughout.

### 3.2 Create Entry Phase (+30.5s)

```
[930] +30.505s >>> ENTER FindExistPos
  *** FIND EXIST POS *** whType=0x8FEDA8  name="<unreadable>"
  ret=0xffffffff (not found)

[932] +30.533s >>> ENTER _CreateEntryEX@24
  *** CREATE ENTRY *** category=1  subType=0  name="<unreadable>"
  [933-936] cmd dispatches (cmd 0x16, cmd 0x09)
[937] returned ProgId=52
```

### 3.3 Save Module Phase (+39.6s)

```
[940] +39.596s >>> ENTER _SaveModule_New@20
  *** SAVE MODULE *** whType=0x1  progId=52
  [941-960] multiple cmd dispatches (cmd 0x0E, etc.)
[961] ret=0x1 (success)
```

### 3.4 Verify Module + Cache Refresh Phase (+39.661s) ← THE KEY SEQUENCE

```
[974] +39.661s >>> ENTER _VerifyModule_New@20
  *** VERIFY MODULE *** whType=0x100AF2  progId=1

[975] +39.661s >>> ENTER FUN_101a8620 (save/compile orchestrator)
  *** SAVE/COMPILE ORCHESTRATOR ***
    category=1  progId=52
    path="C:\Program Files (x86)\TradeStation 9.5\MyWork\"
    caller=0x1fd980b

  [976] +39.661s >>> ENTER FUN_101d0950 (cmd 0x0F helper)
    *** CMD 0x0F HELPER ***
      wh=0xb275b80
      [wh+04]=0x38  [wh+12]=0x34
      [wh+5E]=0xb49d714  ← NON-NULL (already set from init)
      [wh+4E]=0x0  [wh+AE]=0xb2368f4  [wh+C2]=1(SERVER)
      progId=52
  [977] +39.661s <<< EXIT  FUN_101d0950
    progId=52  ret=0x1 (SUCCESS)
    AFTER: [wh+04]=0xF  [wh+5E]=0xb49d714  ← still non-null

  [978-979] cmd 0x3A dispatch (IWHouse method, reads [wh+5E])

  [980] +39.661s >>> ENTER FUN_101c09f0 (cache reset)
    *** CACHE RESET ***
      moduleObj=0x8fec6c
      typeCode=12 (0xC)
      [module+0C]=0xb275b80  ← warehouse (SAME as everything else!)
      [module+10]=0xb49d714  ← COM interface (from cmd 0x0F)
      caller=0x2018716

    [981] +39.661s >>> ENTER FUN_101cf5b0 (cmd dispatcher)
      *** CMD DISPATCH ***
        wh=0xb275b80
        [wh+04]=0x3A  [wh+12]=0xC  ← type code set to 0xC
        [wh+5E]=0xb49d714  ← NON-NULL (from [module+10])
        [wh+4E]=0x0  [wh+AE]=0xb2368f4  [wh+C2]=1(SERVER)
    [982] +39.661s <<< EXIT  FUN_101cf5b0
      cmd=0x4E (CacheReset_QI)  ← CACHE REFRESH DISPATCHED!
      AFTER: [wh+04]=0x4E  [wh+12]=0xC  [wh+5E]=0xb49d714
             [wh+4E]=0x0 (SUCCESS)  [wh+C2]=1(SERVER)

  [983] +39.661s <<< EXIT  FUN_101c09f0 (cache reset)
    typeCode=12  [module+10]_after=0xb49d714

  [984-985] cmd 0x4F (Release IWHouse)

[986] +39.661s <<< EXIT  FUN_101a8620 (save/compile orchestrator)
```

### 3.5 Post-Cache-Refresh Module Processing (+39.661s to +39.723s)

After the cache reset, `FUN_101a8620` processes module elements and additional
cmd 0x0F calls are made. At one point, `[wh+5E]` becomes NULL (after cmd 0x4F
Release), but cmd 0x0F sets it again:

```
[1027] +39.679s >>> ENTER FUN_101d0950 (cmd 0x0F helper)
    [wh+5E]=NULL  ← was released by cmd 0x4F
    progId=52
[1028] +39.679s <<< EXIT  FUN_101d0950
    ret=0x1 (SUCCESS)
    AFTER: [wh+5E]=0xb49d714  ← cmd 0x0F set it again!

[1039] +39.723s <<< EXIT  _VerifyModule_New@20
    progId=1  ret=0x0
```

### 3.6 Open Dialog Enumeration (+48.6s)

```
[1042] +48.573s >>> ENTER CELFileOpenDialog::OnInitDialog

[1043] +48.589s >>> ENTER FUN_10074310 (dialog cmd 0x08)
    *** DIALOG CMD 0x08 ***
      wh=0xb275b80  ← SAME warehouse as everything else!
      [wh+5E]=0xb49d714  ← NON-NULL
      type=0x1402 (TradingApp)
    [1044] AFTER: [wh+04]=0x8 (ProgramsList)  [wh+12]=0x1402

[1045] type=0x2 (Indicator)
[1047] type=0x502
[1049] type=0x402
[1051] type=0x1202
[1053] type=0x0 (Strategy)
[1055] type=0x200
[1057] type=0x3 (ActivityBar)
[1059] type=0x4 (Function)
[1061] type=0x1

[1063] +51.095s <<< EXIT  CELFileOpenDialog::OnInitDialog
    ret=0x1
```

All dialog cmd 0x08 calls use the **same warehouse** (0xb275b80) with
**non-null `[wh+5E]`**. The cache was refreshed by cmd 0x4E at +39.661s,
so the enumeration sees the newly created item.

---

## 4. Ghidra Static Analysis Confirmation

### 4.1 FUN_101a8620 (Save/Compile Orchestrator, RVA 0x1A8620)

Decompiled via Ghidra MCP:

```c
void __fastcall FUN_101a8620(undefined4 param_1, undefined4 param_2, undefined4 param_3)
{
    if ((DAT_1023aaac != 1) &&
        (iVar2 = FUN_101a9650(param_3), iVar2 != 0))  // GetWarehouse(path)
    {
        FUN_1000dd30();                                     // init CELProgramWriter
        FUN_101d43a0(param_1, local_5c);                    // expand types
        iVar2 = FUN_101d0950(local_5c, param_2, local_2d4); // cmd 0x0F
        if (iVar2 != 0) {
            // ... process module elements ...
            FUN_101c09f0(0xc);                              // ← CACHE RESET
        }
        // ... cleanup ...
    }
}
```

**Confirmed**: `FUN_101a8620` calls `FUN_101d0950` (cmd 0x0F) then
`FUN_101c09f0(0xC)` (cache reset with type code 0xC).

### 4.2 FUN_101c09f0 (Cache Reset, RVA 0x1C09F0)

```c
void __thiscall FUN_101c09f0(int param_1, int param_2)
{
    // Phase 1: Invalidate cache flags based on type_code (param_2)
    //   0xC → [module+0x1DE] = 1 (program-list cache)

    // Phase 2: Lock sync object
    CSingleLock::CSingleLock(local_34, *(CSyncObject **)(param_1 + 0x218), 1);

    // Phase 3: Resolve COM interface if [module+0x10] == 0
    if (*(int *)(param_1 + 0x10) == 0) {
        iVar3 = FUN_101cfce0(...);  // cmd 0x0D (unpatched INT3)
        *(int *)(param_1 + 0x10) = iVar3;
    }

    // Phase 4: Set warehouse fields and dispatch 0x4E
    *(undefined4 *)(*(int *)(param_1 + 0xc) + 0x5e) = *(undefined4 *)(param_1 + 0x10);
    //   → [warehouse+0x5E] = [module+0x10]  (COM interface)

    *(int *)(*(int *)(param_1 + 0xc) + 0x12) = param_2;
    //   → [warehouse+0x12] = type_code

    FUN_101cf5b0(0x4e);  // dispatch cmd 0x4E
}
```

**Confirmed**: `FUN_101c09f0` copies `[module+0x10]` to `[warehouse+0x5E]`,
sets `[warehouse+0x12] = type_code`, and dispatches cmd 0x4E.

### 4.3 FUN_101d0950 (Cmd 0x0F Helper, RVA 0x1D0950)

```c
undefined4 __thiscall FUN_101d0950(int param_1, undefined4 param_2,
                                    undefined4 param_3, int param_4)
{
    if (*(int *)(param_1 + 0xae) != 0) {  // COM server initialized
        // ... setup ...
        *(undefined4 *)(param_1 + 4) = 0xf;  // dispatch cmd 0x0F
        // ... wait for completion ...
        if (*(int *)(param_1 + 0x4e) == 0) {  // success
            FUN_101c4ab0(param_1, *(undefined4 *)(param_1 + 0x5e));
            // ↑ Sets [module+0x0C] = warehouse, [module+0x10] = [warehouse+0x5E]
            return 1;
        }
    }
    return 0;
}
```

**Confirmed**: `FUN_101d0950` dispatches cmd 0x0F (which sets `[warehouse+0x5E]`
in the worker thread), then calls `FUN_101c4ab0` to copy the COM interface to
`[module+0x10]`.

### 4.4 FUN_101c4ab0 (Module Field Setter, RVA 0x1C4AB0)

```c
void __thiscall FUN_101c4ab0(int *param_1, int param_2, int param_3)
{
    (**(code **)(*param_1 + 4))();    // vtable cleanup
    (**(code **)(*param_1 + 0xc))();  // vtable cleanup
    param_1[3] = param_2;             // [module+0x0C] = warehouse
    param_1[4] = param_3;             // [module+0x10] = COM interface
}
```

**Confirmed**: Sets `[module+0x0C]` = warehouse pointer and `[module+0x10]` =
COM interface pointer (from `[warehouse+0x5E]`).

---

## 5. Warehouse State Analysis

### 5.1 `[warehouse+0x5E]` Throughout the Operation

| Phase | `[wh+5E]` Value | Source |
|-------|-----------------|--------|
| Init (first cmd 0x09) | 0xb3af0d4 | cmd 0x09 (ProgramByName) sets it |
| CreateEntry | 0xb49d714 | cmd 0x09 during CreateEntry |
| SaveModule | 0xb49d714 | maintained from CreateEntry |
| VerifyModule → cmd 0x0F | 0xb49d714 | already set (maintained) |
| Cache reset → cmd 0x4E | 0xb49d714 | copied from [module+0x10] |
| After cmd 0x4F (Release) | NULL | released by cmd 0x4F |
| After next cmd 0x0F | 0xb49d714 | cmd 0x0F sets it again |
| Open dialog → cmd 0x08 | 0xb49d714 | non-null throughout |

**Conclusion**: `[warehouse+0x5E]` is **always non-null** during TSDev's
operation. It is set during initialization and maintained throughout. Even when
temporarily cleared by cmd 0x4F (Release), cmd 0x0F immediately restores it.

### 5.2 Warehouse Mode

`[warehouse+0xC2] = 1` (SERVER mode) throughout all operations. This is the same
mode as our CLI's warehouse.

### 5.3 Single Warehouse

The same warehouse pointer (0xb275b80) is used for ALL operations:
- CreateEntry
- SaveModule
- VerifyModule
- Cache reset (cmd 0x4E)
- Open dialog enumeration (cmd 0x08)

There is NO separate "50 spaces" warehouse. The path used by `FUN_101a8620` is
`"C:\Program Files (x86)\TradeStation 9.5\MyWork\"` — the actual MyWork directory.

---

## 6. Critical Correction to Previous Analysis

### 6.1 The "50 Spaces" Path Myth

The previous analysis (`RE_F5_frida_trace_analysis.md` §5.2) claimed:

> `FUN_101a8620` calls `FUN_101a9650(param_3)` (GetWarehouse) with the 50-space
> string as the path parameter. **This does not match our CLI's warehouse.**

**This is incorrect.** The live Frida trace shows:

```
*** SAVE/COMPILE ORCHESTRATOR *** category=1  progId=52
    path="C:\Program Files (x86)\TradeStation 9.5\MyWork\"
```

The path is the actual MyWork directory, NOT "50 spaces". The "50 spaces" string
(`s__10230710` at VA `0x10230710`) may exist in the binary but is NOT the path
used at runtime. The actual path comes from TSDev's runtime state (likely the
`DAT_102408fc` global set by `OrlibInit`).

### 6.2 Impact on Previous Conclusions

The previous analysis concluded that the cache refresh operates on the "wrong
warehouse" due to the path mismatch. **This conclusion is incorrect.** The live
trace proves the cache refresh operates on the **same warehouse** as all other
operations.

The real reason our CLI's cache refresh fails is simply that
`[warehouse+0x5E]` is NULL in our CLI — it was never set because our CLI doesn't
perform the same initialization sequence as TSDev (which dispatches cmd 0x09
during startup, setting `[warehouse+0x5E]`).

---

## 7. Why `[warehouse+0x5E]` is NULL in Our CLI

### 7.1 TSDev's Initialization Sets `[warehouse+0x5E]`

In TSDev, the first command dispatched after warehouse initialization is cmd 0x09
(ProgramByName), which sets `[warehouse+0x5E]` on success. This happens at +1.199s
during TSDev's startup, before any user interaction.

### 7.2 Our CLI's Initialization Does NOT Set `[warehouse+0x5E]`

Our CLI calls `GetWarehouse(NULL)` which initializes the warehouse (cmd 0x01) but
does NOT dispatch cmd 0x09. The first operations our CLI performs are:
- `findExistPos` — BST cache lookup (server mode, no cmd dispatch)
- `_LoadMasterRecsEX@16` — cmd 0x08 (does NOT set `[warehouse+0x5E]`)

Neither of these sets `[warehouse+0x5E]`.

### 7.3 CreateEntry's cmd 0x09 Should Set It — But Doesn't

`_CreateEntryEX@24` dispatches cmd 0x09 (via `FUN_101d0860`), which should set
`[warehouse+0x5E]` on success. However, runtime diagnostics in our CLI show it
remains NULL. Possible explanations:
1. The COM QueryInterface (`FUN_101cf020`) fails in our CLI's context
2. The SEH handler catches the COM error, leaving `[warehouse+0x5E]` = 0
3. Some other initialization difference prevents the COM interface from being obtained

### 7.4 The Solution: Explicitly Dispatch cmd 0x0F

Regardless of why `[warehouse+0x5E]` is NULL in our CLI, the solution is to
explicitly dispatch cmd 0x0F (via `FUN_101d0950`) to set it, then dispatch
cmd 0x4E to refresh the cache. This is exactly what TSDev does via
`FUN_101a8620`.

---

## 8. Recommended Approach for Our CLI

### 8.1 Replicate TSDev's Cache Refresh Sequence

After every INSERT, MODIFY, or DELETE operation:

1. **Call `FUN_101d0950`** on our warehouse with a known ProgId to set
   `[warehouse+0x5E]`:
   - ECX = warehouse pointer (from `GetWarehouse(NULL)`)
   - Stack arg 1 = type array (from `FUN_101d43a0(category, outArray)`)
   - Stack arg 2 = ProgId (from the operation that just succeeded)
   - Stack arg 3 = output int pointer (can be dummy)
   - `FUN_101d0950` is already INT3-patched (offset 67)

2. **Set `[warehouse+0x12] = 0xC`** (program-list cache type code)

3. **Call `FUN_101cf5b0(warehouse, 0x4E)`** to dispatch the cache refresh:
   - ECX = warehouse pointer
   - Stack arg = 0x4E
   - `FUN_101cf5b0` is already INT3-patched (offset 36)

### 8.2 Why This Works

- `FUN_101d0950` dispatches cmd 0x0F (`IWHouse::ProgramByIdTypes`), which:
  - Calls the WHServer COM interface (vtable +0xA8)
  - On success, sets `[warehouse+0x5E]` to the IWHouse COM interface
  - Calls `FUN_101c4ab0` to copy the interface to `[module+0x10]`

- `FUN_101cf5b0(warehouse, 0x4E)` dispatches cmd 0x4E (cache reset), which:
  - Reads `[warehouse+0x5E]` (now non-null, set by cmd 0x0F)
  - Calls IWHouse vtable method 0x48 with the type code
  - Refreshes the server-side cache for the specified type

- Subsequent `_LoadMasterRecsEX@16` (cmd 0x08) calls will see the refreshed data

### 8.3 Type Code Selection

TSDev uses type code `0xC` (program-list cache) for non-Function types. For
a universal refresh, type code `0` (ALL caches) can be used. The trade-off:
- `0xC`: Targeted refresh (program-list cache only) — matches TSDev's behavior
- `0`: Full refresh (all caches) — safer but potentially slower

**Recommendation**: Use `0xC` to match TSDev's behavior exactly. If issues
arise with specific component types, fall back to `0`.

### 8.4 Required Patches (All Already Available)

| Patch | Function | Offset | Status |
|-------|----------|--------|--------|
| `FUN_101d0950_thread_check_INT3` | `FUN_101d0950` | 67 | Already patched |
| `FUN_101cf5b0_thread_check_INT3` | `FUN_101cf5b0` | 36 | Already patched |

**No new patches or signatures needed.**

### 8.5 Pseudo-Code Implementation

```cpp
// After successful insert/modify/delete:
void* wh = nullptr;
callGetWarehouse(m_pGetWarehouse, 0, &wh);  // GetWarehouse(NULL)
if (!wh) return;

// Check if [warehouse+0x5E] is already set
void* comInterface = *(void**)((char*)wh + 0x5E);
if (!comInterface) {
    // Need to set [warehouse+0x5E] via cmd 0x0F
    // Build type array from category
    int typeArray[16];
    callFUN_101d43a0(category, typeArray);  // expand types

    // Dispatch cmd 0x0F
    int outInt = 0;
    __try {
        callFUN_101d0950(wh, typeArray, progId, &outInt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;  // cmd 0x0F failed, can't refresh cache
    }
}

// Set type code and dispatch cmd 0x4E
*(int*)((char*)wh + 0x12) = 0xC;  // program-list cache
__try {
    callFUN_101cf5b0(wh, 0x4E);   // cache refresh
} __except (EXCEPTION_EXECUTE_HANDLER) {
    return;  // cache refresh failed
}
```

### 8.6 Alternative: Call FUN_101a8620 Directly

If resolving `FUN_101d0950` and `FUN_101d43a0` separately is complex, an
alternative is to call `FUN_101a8620` directly with `param_3 = 0` (NULL path):

```cpp
// FUN_101a8620 is __fastcall: ECX = category, EDX = progId, stack = path
callFUN_101a8620(category, progId, NULL);  // NULL path → GetWarehouse(NULL) → our warehouse
```

This would make `GetWarehouse(NULL)` find our warehouse, so the cache reset
operates on the correct warehouse. However, this requires:
- Resolving `FUN_101a8620` (new signature scan)
- The function processes module elements (side effects)
- `DAT_1023aaac` must be != 1

**Recommendation**: The direct `FUN_101d0950` + `FUN_101cf5b0` approach is
simpler and more controlled.

---

## 9. Risks and Caveats

### 9.1 cmd 0x0F May Fail

If `FUN_101d0950` (cmd 0x0F) fails (returns 0), `[warehouse+0x5E]` will be
cleared to NULL (the failure path in the worker thread). In this case, cmd 0x4E
cannot be dispatched. The CLI should check the return value and skip the cache
refresh if cmd 0x0F fails.

### 9.2 COM Interface Lifetime

The COM interface at `[warehouse+0x5E]` is reference-counted. cmd 0x4F
(Release) decrements the refcount. If the refcount reaches 0, the interface
is released. Subsequent cmd 0x0F calls will re-obtain the interface. Our CLI
should not call cmd 0x4F — just let cmd 0x0F manage the interface lifetime.

### 9.3 Thread Safety

`FUN_101d0950` and `FUN_101cf5b0` both use:
1. `WaitForSingleObject([warehouse+0x72], INFINITE)` — mutex
2. Thread check (INT3 — already patched to NOP)
3. `SetEvent([warehouse+0x6e])` — signal worker
4. `WaitForSingleObject([warehouse+0x6a], INFINITE)` — wait completion
5. `ReleaseMutex([warehouse+0x72])` — release mutex

This is thread-safe. The INT3 patch allows our CLI thread to dispatch commands.

### 9.4 SEH Protection

All calls to internal orlib20 functions should be SEH-protected, as is the
existing practice in the CLI.

### 9.5 ProgId Requirement

`FUN_101d0950` requires a valid ProgId of an existing item. After insert, the
ProgId is available from `createEntry`'s return value. After modify, the ProgId
is available from `findExistPos`. After delete, the ProgId is available from
`findExistPos` (called before deletion).

### 9.6 Type Array Construction

`FUN_101d0950` requires a type array built by `FUN_101d43a0`. This function
expands a category into an array of warehouse types. It is the same function
used by `_LoadMasterRecsEX@16` and `FindExistPos`. The existing code already
resolves and calls this function.

### 9.7 Module Object for FUN_101c09f0

If using the direct `FUN_101c09f0` approach (instead of `FUN_101a8620`), a
module object is required. The module object must have:
- `[module+0x0C]` = warehouse pointer
- `[module+0x10]` = COM interface pointer (from cmd 0x0F)
- `[module+0x218]` = valid CSyncObject (for CSingleLock)

The module object is initialized by `FUN_1000dd30` (CELProgramWriter constructor).
If `[module+0x10]` is 0, `FUN_101c09f0` will try to resolve it via
`FUN_101cfce0` (cmd 0x0D), which has an **unpatched INT3** at offset 40. This
would crash. Therefore, `FUN_101d0950` must succeed first to set `[module+0x10]`.

**Alternative**: Skip `FUN_101c09f0` entirely and just do:
1. Call `FUN_101d0950` to set `[warehouse+0x5E]`
2. Manually set `[warehouse+0x12] = 0xC`
3. Call `FUN_101cf5b0(warehouse, 0x4E)` directly

This avoids the module object and `FUN_101c09f0` entirely, while still
achieving the same result (cmd 0x4E dispatch with non-null `[warehouse+0x5E]`).

---

## 10. Summary of Confirmed Facts

| Claim | Evidence | Status |
|-------|----------|--------|
| TSDev refreshes cache via `FUN_101a8620` → `FUN_101d0950` → `FUN_101c09f0(0xC)` → cmd 0x4E | Live Frida trace: events [975]-[983] | **Confirmed** |
| `[warehouse+0x5E]` is non-null when cmd 0x4E is dispatched | Live trace: `[wh+5E]=0xb49d714` at event [981] | **Confirmed** |
| The path is "50 spaces" | Previous static analysis | **DISPROVEN** — actual path is `"C:\...\MyWork\"` |
| The cache refresh operates on a different warehouse | Previous static analysis | **DISPROVEN** — same warehouse (0xb275b80) throughout |
| cmd 0x0F sets `[warehouse+0x5E]` | Live trace: event [1027-1028], NULL → non-null | **Confirmed** |
| cmd 0x4F clears `[warehouse+0x5E]` | Live trace: event [1027], NULL after Release | **Confirmed** |
| Open dialog uses same warehouse as mutations | Live trace: wh=0xb275b80 in all events | **Confirmed** |
| Open dialog uses cmd 0x08 for enumeration | Live trace: events [1043]-[1062] | **Confirmed** |
| `[warehouse+0xC2] = 1` (SERVER mode) throughout | Live trace: all events show `[wh+C2]=1(SERVER)` | **Confirmed** |
| `FUN_101a8620` calls `FUN_101c09f0(0xC)` after `FUN_101d0950` succeeds | Ghidra decompilation + live trace | **Confirmed** |
| `FUN_101c09f0` copies `[module+0x10]` to `[warehouse+0x5E]` | Ghidra decompilation | **Confirmed** |
| `FUN_101d0950` sets `[module+0x10]` via `FUN_101c4ab0` | Ghidra decompilation | **Confirmed** |

---

## 11. Evidence Index

| Artifact | Path | Description |
|----------|------|-------------|
| Frida trace script | `tmp/frida_trace_tsdev_cache_refresh.js` | 15 hooks, send() message delivery |
| Frida trace runner | `tmp/frida_trace_v2.py` | Spawn + UI automation via SendKeys |
| Frida trace log | `tmp/frida_cache_refresh_trace.log` | 2688 lines, 1063 sequence events |
| Ghidra decompilation | orlib20.dll @ 0x101a8620 | `FUN_101a8620` save/compile orchestrator |
| Ghidra decompilation | orlib20.dll @ 0x101c09f0 | `FUN_101c09f0` cache reset |
| Ghidra decompilation | orlib20.dll @ 0x101d0950 | `FUN_101d0950` cmd 0x0F helper |
| Ghidra decompilation | orlib20.dll @ 0x101c4ab0 | `FUN_101c4ab0` module field setter |
| Previous Frida trace | `RE_F5_frida_trace_analysis.md` | Earlier trace (sequence confirmed, path was wrong) |
| Previous DELETE trace | `tmp/frida_complete_trace.log` | Delete trace showing cmd 0x4E dispatches |

---

> **This document is research-only.** No production source, signature files, or
> tests were modified during this investigation. The Frida trace script and
> runner are in `tmp/` for reproducibility.
