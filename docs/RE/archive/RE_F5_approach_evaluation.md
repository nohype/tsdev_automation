# RE F5: Approach Evaluation — Approach F (Cache Refresh) vs Hybrid Approach

## 1. Purpose

This document evaluates whether **Approach F** (calling `FUN_101a8620` with NULL path
to trigger a cache refresh on our warehouse) or the **Hybrid approach** (cached
enumeration + live lookup for session-created items) is better for TVS-F.5
(case-insensitive `exists` via enumeration).

All claims from `RE_F5_frida_trace_analysis.md` were independently verified via
Ghidra decompilation. This is a research-only document — no production source,
signature files, or tests were modified.

---

## 2. Verification Results

### 2.1 FUN_101a8620 (RVA 0x1A8620) — VERIFIED

**Decompilation confirmed** the function exists at RVA 0x1A8620 and is a
`__fastcall` orchestrator with 3 parameters:

- **ECX (param_1)**: warehouse type / category
- **EDX (param_2)**: ProgId
- **Stack (param_3)**: path string (wide pointer, passed to GetWarehouse)

**Key structure verified:**
```c
void __fastcall FUN_101a8620(param_1, param_2, param_3) {
    if (DAT_1023aaac != 1 &&
        (iVar2 = FUN_101a9650(param_3), iVar2 != 0))  // GetWarehouse(param_3)
    {
        FUN_1000dd30();                                // init CELProgramWriter
        FUN_101d43a0(param_1, local_5c);               // expand types
        iVar2 = FUN_101d0950(local_5c, param_2, local_2d4); // cmd 0x0F
        if (iVar2 != 0) {
            // Module element processing loop
            iVar2 = FUN_101a8750(extraout_ECX);        // get program object
            local_20 = FUN_101e5970();                 // get element count
            for (local_14 = 0; local_14 < local_20; local_14++) {
                FUN_101e12f0(local_14);                // get element type
                FUN_101e12a0(local_14);                // get element name
                FUN_101e1150(local_14);                // resolve element
                FUN_101a4f90(param_1, param_2);        // process element
            }
            if (iVar2 != 0) FUN_10006a00();            // cleanup
            FUN_101c09f0(0xc);                         // ← CACHE RESET
        }
        // CELProgramWriter dtor cleanup
        FUN_101c0fd0();
        FUN_101c00c0();
    }
}
```

**Assembly confirms** `__fastcall` parameter passing:
- `MOV EBX, EDX` (param_2 from EDX)
- `MOV ESI, ECX` (param_1 from ECX)
- `PUSH dword ptr [EBP + 0x8]` (param_3 from stack)

**Calling convention nuance**: The function ends with `RET` (not `RET 4`), and
all three callers (`_VerifyModule_New@20`, `FUN_10159ae0`, `FUN_10168ee0`) clean
up the stack parameter with `ADD ESP, 0x4` after the call. This means the
**caller** cleans up the single stack parameter — this is NOT standard MSVC
`__fastcall` (which uses callee-cleanup). It is a custom convention: ECX/EDX for
the first two params, caller-cleanup for remaining stack params. See §4.2 for
calling implications.

### 2.2 GetWarehouse(param_3) with NULL — VERIFIED

`FUN_101a9650` (GetWarehouse, RVA 0x1A9650) decompilation confirms:

- **param_1 == 0 (NULL)**: Uses `DAT_102408fc` as the search key — this is the
  CSIDL_PROGRAM_FILES_COMMON path (0x4a), set once via
  `CTsEnvironment::GetFolderPath(0x4a, 0)` and stored as an ANSI CString.
- **param_1 != 0**: Converts the wide string to ANSI via `FUN_10009dc0`,
  strips trailing backslash, uses it as the search key.
- Searches the warehouse list (`DAT_1023faf0` array) with `lstrcmpiA` for
  the current thread only (`GetCurrentThreadId()` check).
- If no match found, **creates a new warehouse**.

**Conclusion**: Calling `FUN_101a8620(category, progId, 0)` with param_3 = 0
would cause `GetWarehouse(NULL)` to search for the CSIDL path — which is how our
CLI's warehouse was registered. This would find our warehouse, and the cache
reset would operate on the correct warehouse. **VERIFIED.**

### 2.3 FUN_101c09f0 (RVA 0x1C09F0) — VERIFIED

Decompilation confirms all claims:
- `__thiscall`: ECX = module object, stack = type code
- Type code 0xC → invalidates `[module+0x1DE]` (program-list cache) via
  `FUN_101e0fb0()`
- `CSingleLock` on `[module+0x218]` sync object
- `FUN_101cfce0` (cmd 0x0D, unpatched INT3) only called when `[module+0x10] == 0`
  — skipped in normal flow because `FUN_101d0950` sets `[module+0x10]` via
  `FUN_101c4ab0`
- Sets `[warehouse+0x5E]` = COM interface, `[warehouse+0x12]` = type_code
- Dispatches `FUN_101cf5b0(0x4E)` (cache refresh)

### 2.4 _GetMasterRecByType@16 (RVA 0x1A0460) — VERIFIED

Decompilation confirms:
- **Ordinal 510** (comment in decompilation: `510  _GetMasterRecByType@16`)
- `FUN_101a9650(0)` — GetWarehouse(NULL) → uses OUR warehouse ✓
- `FUN_101d4280(param_1, param_2)` — expands category+subtype to warehouse type
- `FUN_101cfe00(uVar2, param_3, 0)` — live lookup by expanded type + index (ProgId)
- Copies 0x42 dwords (0x108 bytes) + 2 bytes = **0x10A (266) bytes** to outBuf
- Returns 1 on success, 0 on failure (not found / warehouse unavailable)
- `__stdcall` (4 stack params, callee cleanup via `RET 0x10`)

### 2.5 FUN_101d15c0 (RVA 0x1D15C0) — VERIFIED

**INT3 at offset 84**: Confirmed. The instruction at 0x101D1614 (offset 0x54 = 84
from function start 0x101D15C0) is `INT3` (opcode 0xCC).

**Byte pattern uniqueness**: The 97-byte signature pattern from the analysis
document was searched via `search_byte_patterns` with wildcards. Result:
**exactly 1 match** at 0x101D15C0. The pattern is unique and will reliably
resolve to FUN_101d15c0.

**Verified byte at offset 84**: The raw bytes at 0x101D15C0 were read (97 bytes).
Byte at index 84 is `0xCC` (INT3). The full pattern matches the signature with
`??` wildcards at the expected positions (PUSH immediate, JZ relative offset,
two CALL indirect targets).

### 2.6 DAT_1023aaac Guard — VERIFIED

`DAT_1023aaac` is a global "verification in progress" flag:
- Set to 1 by `_VerifyModuleEX@28` before calling `FUN_10168ee0` (a caller of
  `FUN_101a8620`), then restored to its previous value afterward.
- When `DAT_1023aaac == 1`, `FUN_101a8620` skips entirely (the guard
  `DAT_1023aaac != 1` fails).
- After `verifyModule` returns, `DAT_1023aaac` is restored to its previous value
  (likely 0), so a subsequent call to `FUN_101a8620` would proceed.

---

## 3. Approach F Safety Assessment

### 3.1 What does FUN_101a8620 do AFTER the cache reset?

The cache reset (`FUN_101c09f0(0xc)`) is the **last significant action** before
cleanup. After the cache reset, the function only does:
1. `FUN_101c0fd0()` — CELProgramWriter cleanup
2. `FUN_101c00c0()` — CELProgramWriter destructor

These are lifecycle methods for the stack-allocated CELProgramWriter object
(local at `[EBP-0x2D0]`). They are safe — they just clean up the local object.

### 3.2 What does the module element processing loop do?

The loop (between `FUN_101d0950` and `FUN_101c09f0`) processes each module
element of the program being saved/compiled:

1. `FUN_101e5970()` — gets the element count from the program object
2. For each element (0 to count-1):
   - `FUN_101e12f0(index)` — retrieves the element's type code (getter, no mutation)
   - `FUN_101e12a0(index)` — retrieves the element's name string (getter, no mutation)
   - `FUN_101e1150(index)` — resolves the element by type+name via `FUN_101d0540`,
     then calls `FUN_101c2e90` and releases the COM object (lookup-and-release)
   - `FUN_101a4f90(category, progId, typeCode, name)` — **a mini save/compile cycle**:
     calls `GetWarehouse(NULL)`, `FUN_101d0590` (get program object),
     `FUN_101d0950` (cmd 0x0F, load program), `FUN_101c36d0`/`FUN_101c4990`/
     `FUN_101c6a40` (compile/verify steps), then cleanup

**This is NOT a no-op.** The loop re-loads and re-processes each module element
(dependency) of the program. `FUN_101a4f90` is itself a mini save/compile cycle
that calls cmd 0x0F again for each dependency.

### 3.3 Is the loop safe to call after verifyModule?

**Partially.** If the program was already successfully compiled and verified by
`verifyModule`, the module elements (dependencies) are already in their final
state. Re-processing them should be idempotent — the compilation steps would
find the program already compiled and either skip or recompile to the same state.

However, there are risks:
1. **Performance**: The loop calls cmd 0x0F for each dependency, which involves
   COM calls to the warehouse server. For a program with many dependencies, this
   could be slow.
2. **Side effects**: `FUN_101a4f90` calls `FUN_101c6a40` (compile/verify), which
   could modify compiler state or trigger additional cache invalidations.
3. **Error handling**: If any dependency lookup fails (e.g., a dependency was
   deleted), `FUN_101a4f90` may handle it gracefully (it checks `iVar2 == 1`),
   but this is unconfirmed for all edge cases.

### 3.4 Does FUN_101a8620 require specific state?

Yes:
1. **DAT_1023aaac != 1**: Must not be in "verification in progress" state. After
   `verifyModule` returns, this is restored to 0, so this is satisfied.
2. **GetWarehouse(NULL) must succeed**: Our warehouse must be registered. This
   is satisfied after `OrlibInit` + any `GetWarehouse(NULL)` call.
3. **FUN_101d0950 (cmd 0x0F) must succeed**: The ProgId must exist in the
   warehouse. If it doesn't, the entire function (including cache reset) is
   skipped — safe failure mode.
4. **CELProgramWriter initialization**: `FUN_1000dd30` initializes a
   stack-local CELProgramWriter. This is self-contained — no external state
   needed.
5. **No CELProgramWriter needs to be initialized elsewhere**: The function
   creates and destroys its own CELProgramWriter on the stack. It does NOT
   require an externally-initialized one.

### 3.5 What happens if FUN_101d0950 (cmd 0x0F) fails?

If `FUN_101d0950` returns 0 (failure), the entire `if (iVar2 != 0)` block is
skipped — including the module element loop AND the cache reset
(`FUN_101c09f0(0xc)`). The function proceeds directly to CELProgramWriter
cleanup and returns.

**This is a safe failure mode**: no cache reset occurs, no module processing
occurs, and the function cleans up and returns normally.

### 3.6 Could calling FUN_101a8620 with NULL path have negative side effects?

**Potential concerns:**
1. **Module element reprocessing**: As discussed in §3.2-3.3, the loop re-processes
   dependencies. This is the main risk — it could be slow or trigger unexpected
   compilation side effects.
2. **FUN_101a8750 creates a program object**: This calls `GetWarehouse(NULL)`,
   `FUN_101d0590` (cmd 0x0F variant), and `FUN_101c2710` (CELPrograms init).
   The resulting object is cleaned up (refcount decrement + release).
3. **FUN_10006a00 cleanup**: Called if `FUN_101a8750` returns non-zero. This is
   a cleanup routine — should be safe.
4. **No direct warehouse mutation**: The function does not directly modify the
   warehouse — it only reads from it (via cmd 0x0F) and then triggers the cache
   reset (which refreshes the server-side cache, not a destructive operation).

**No direct negative side effects on the warehouse data** are apparent. The
cache refresh (cmd 0x4E) is a non-destructive operation that tells the server to
refresh its cache for the specified type.

### 3.7 Calling convention for C++

The function uses a custom convention: ECX/EDX for first two params, with
caller-cleanup for the stack param (the function ends with `RET`, not `RET 4`).

This means we **cannot** use a simple `__fastcall` typedef — MSVC's `__fastcall`
would expect the callee to clean up the stack param, leading to a stack imbalance.

**Solution**: Use inline assembly, following the existing pattern in
`tsdev_lib.cpp` (see `callDeleteOrchestrator`):

```cpp
#pragma warning(push)
#pragma warning(disable: 4731)
bool callCacheRefresh(void* fnPtr, int category, int progId) {
    __try {
        __asm {
            push 0                  ; param_3 = NULL (GetWarehouse(NULL))
            mov edx, progId         ; param_2 = ProgId
            mov ecx, category       ; param_1 = category
            call fnPtr
            add esp, 4              ; caller-cleanup of stack param
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#pragma warning(pop)
```

---

## 4. Hybrid Approach Completeness Assessment

### 4.1 CLI use case (separate process per command)

For the typical CLI use case (`tsdev_cli exists --case-insensitive`), each
command spawns a **fresh process**. The process calls `OrlibInit`, which
initializes the warehouse cache from the current server state. The cache is
fresh — it sees all items that exist at that moment, including items created by
TSDev or previous CLI runs.

**Cache staleness does NOT matter** for the CLI use case. The cached
`_LoadMasterRecsEX@16` enumeration sees the current state.

### 4.2 Python wrapper (TsDevClient) use case

The Python wrapper drives the C++ CLI as a **subprocess**. Each `exists` call
spawns a new `tsdev_cli.exe` process. There is no batch-exists mode.

**Cache staleness does NOT matter** for the Python wrapper use case either —
each call is a fresh process.

### 4.3 Test use case (test_enumerate.cpp)

The test creates an item (`createEntry` + `saveSource` + `verifyModule`) and
then calls `enumerateRecords` **in the same process**. The cached
`_LoadMasterRecsEX@16` was populated at `OrlibInit` time (before the item was
created), so it does NOT see the newly created item.

**Cache staleness DOES matter** for the test use case. This is the primary
scenario where the cache refresh is needed.

### 4.4 Would the Hybrid approach fix the test?

**Yes.** The Hybrid approach would work as follows:
1. Call `_LoadMasterRecsEX@16` for the baseline (all items at init time — 1476
   records, including TSDev-created items).
2. Call `_GetMasterRecByType@16` to look up the session-created item by its
   known ProgId (the test knows the ProgId from `createEntry`).
3. Combine the two lists.

The test knows the ProgId (from `createEntry` return value), so it can do a
live lookup for that specific item. The Hybrid approach would find the item.

### 4.5 Does the Hybrid approach see items created by other processes after our init?

**No.** The Hybrid approach sees:
- All items at init time (from cached enumeration)
- Items created in the current session (from live lookup by known ProgId)

It does NOT see items created by TSDev or other processes AFTER our `OrlibInit`
but BEFORE our enumeration. However, this is an **acceptable limitation** for
the `exists` use case:
- The CLI/Python use case spawns a fresh process per command, so init time = now.
- The test use case only needs to find items it created in the same session.

### 4.6 Limitation: requires tracking session ProgIds

The Hybrid approach requires knowing the ProgIds of items created in the current
session. In the test, this is trivial (the test captures the ProgId from
`createEntry`). In a general `enumerateRecords` implementation, the library
would need to track all ProgIds assigned during the session.

**For the `exists --case-insensitive` use case**, the caller knows the name
they're looking for. If the item was created in this session, the caller likely
knows the ProgId. But for a general enumeration (listing all items), this
becomes more complex — you'd need to iterate ProgId space, which has gap
issues (see §6.4 of the analysis document).

---

## 5. Recommendation

### **Primary Recommendation: Hybrid Approach**

The Hybrid approach is recommended over Approach F for TVS-F.5, for the
following reasons:

1. **Safety**: The Hybrid approach uses only exported functions
   (`_LoadMasterRecsEX@16` and `_GetMasterRecByType@16`) — no internal function
   calls, no module object construction, no COM interface manipulation. Approach
   F calls `FUN_101a8620`, which runs a full save/compile cycle including
   dependency reprocessing — this has unconfirmed side effects.

2. **Simplicity**: The Hybrid approach requires only 1 new patch
   (`FUN_101d15c0` INT3 at offset 84) and 1 new export resolution
   (`_GetMasterRecByType@16`). Approach F requires a new signature scan for
   `FUN_101a8620`, inline assembly for the custom calling convention, and
   careful state management.

3. **Adequate for the use case**: The cache staleness problem only matters in
   the test scenario (same-process create + enumerate). The CLI and Python
   wrapper use cases spawn fresh processes and are unaffected. The Hybrid
   approach fully solves the test scenario.

4. **No side effects**: The Hybrid approach is read-only — it queries the
   warehouse without modifying any state. Approach F triggers a cache refresh
   (cmd 0x4E) which, while non-destructive, is a server-side operation with
   potential performance implications and unconfirmed behavior across all
   component types.

5. **Reliability**: `_GetMasterRecByType@16` is an exported function designed
   for external use. `FUN_101a8620` is an internal orchestrator not designed
   for direct external invocation.

### When Approach F would be preferred

Approach F would be preferred if:
- We needed a general "list all items" enumeration that sees items created by
  OTHER processes after our init (the Hybrid approach can't do this).
- We wanted the cached `_LoadMasterRecsEX@16` to always reflect current state
  (refresh-on-demand pattern).
- The module element reprocessing was confirmed to be safe and idempotent across
  all component types.

### Simpler approach considered

**Is there a simpler approach we're missing?** Yes — for the specific
`exists --case-insensitive` use case, we could:

1. Use `findExistPos` (exact match, already working) for the common case.
2. If exact match fails, fall back to `_LoadMasterRecsEX@16` enumeration
   (cached, sees all init-time items) for case-insensitive search.
3. If the cached enumeration also fails AND we're in a same-process create+
   enumerate scenario, do a live lookup via `_GetMasterRecByType@16` for the
   known ProgId.

This is essentially the Hybrid approach, but optimized: the live lookup is only
needed as a fallback for session-created items, not for every enumeration.

---

## 6. Implementation Guidance (Hybrid Approach)

### 6.1 Add Export Entry

Add `_GetMasterRecByType@16` to the signature JSON exports array:

```json
{
  "name": "_GetMasterRecByType@16",
  "rva": "0x1A0460",
  "callingConvention": "stdcall"
}
```

### 6.2 Add INT3 Patch Signature

Add the `FUN_101d15c0_thread_check_INT3` patch signature (verified unique, 1
match). The hex pattern and patch details from the analysis document (§8.2) are
confirmed correct.

### 6.3 Type Definition

```cpp
// _GetMasterRecByType@16 — __stdcall, 4 params (category, subtype, index, outBuf)
// Returns 1 on success (record found), 0 on failure (not found / warehouse unavailable)
// outBuf must be 0x10A (266) bytes, caller-allocated
// index is the ProgId (NOT a sequential position)
typedef int (__stdcall *GetMasterRecByTypeFn)(int category, int subType,
                                              int index, void* outBuf);
```

### 6.4 Subtype Mapping

Use the existing `getSubType(type)` function:
- Strategy: 0 → expanded type 0
- Indicator: 0 → expanded type 2
- Function: 0 → expanded type 4
- TradingApp: 0x14 → expanded type 0x1402

### 6.5 Hybrid Enumeration Algorithm

```cpp
EnumerateResult enumerateRecordsHybrid(ComponentType type,
                                        const std::string& searchName,
                                        int sessionProgId = -1) {
    EnumerateResult result;

    // 1. Cached enumeration (sees all items at init time)
    result = enumerateRecords(type, searchName);  // existing _LoadMasterRecsEX@16
    if (result.exists) return result;

    // 2. Live lookup for session-created item (if ProgId known)
    if (sessionProgId > 0) {
        int category = getCategory(type);
        int subType = getSubType(type);
        char buf[0x10A] = {};
        if (callGetMasterRecByType(m_getMasterRecByType, category, subType,
                                    sessionProgId, buf, &seh)) {
            if (seh.result == 1) {
                const char* storedName = buf + 0x17;
                if (_stricmp(storedName, searchName.c_str()) == 0) {
                    result.exists = true;
                    result.storedName = std::string(storedName);
                    result.progId = *(short*)(buf + 0x00);
                    return result;
                }
            }
        }
    }

    return result;  // not found
}
```

### 6.6 Tracking Session ProgIds

For the general case (not just the test), the library should track ProgIds
assigned during the session:

```cpp
// In TsDevLib class:
std::vector<int> m_sessionProgIds;  // ProgIds created in this session

// After createEntry:
int progId = ...;  // from createEntry
if (progId > 0) m_sessionProgIds.push_back(progId);
```

Then in `enumerateRecordsHybrid`, iterate over `m_sessionProgIds` instead of a
single `sessionProgId`:

```cpp
// 2. Live lookup for all session-created items
for (int pid : m_sessionProgIds) {
    char buf[0x10A] = {};
    if (callGetMasterRecByType(..., pid, buf, &seh) && seh.result == 1) {
        if (_stricmp(buf + 0x17, searchName.c_str()) == 0) {
            // Found!
        }
    }
}
```

### 6.7 Patch Restoration

The `m_threadCheckRvas` array (currently size 6) must be increased to 7 to
include the new `FUN_101d15c0` patch site.

### 6.8 SEH-Protected Call Wrapper

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

## 7. Implementation Guidance (Approach F — if chosen instead)

### 7.1 Signature Scan

A new signature scan is needed for `FUN_101a8620` at RVA 0x1A8620. The function
prologue is standard MSVC SEH frame setup — not unique enough for a byte
pattern. A structural signature (matching specific call targets like
`FUN_101a9650`, `FUN_101d0950`, `FUN_101c09f0`) would be more reliable.

### 7.2 Calling Convention

Use inline assembly (NOT a `__fastcall` typedef) due to the caller-cleanup
convention:

```cpp
#pragma warning(push)
#pragma warning(disable: 4731)
bool callCacheRefresh(void* fnPtr, int category, int progId) {
    __try {
        __asm {
            push 0                  ; param_3 = NULL
            mov edx, progId         ; param_2 = ProgId
            mov ecx, category       ; param_1 = category
            call fnPtr
            add esp, 4              ; caller-cleanup
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#pragma warning(pop)
```

### 7.3 Safety Measures

1. **Call after verifyModule**: Ensures the ProgId exists and the program is
   compiled. `DAT_1023aaac` will be 0 (restored by `_VerifyModuleEX@28`).
2. **SEH protection**: Wrap the call in `__try/__except` to catch any crashes.
3. **Check return**: The function returns void, so success must be inferred from
   subsequent enumeration results.
4. **Use a valid ProgId**: The ProgId must exist in the warehouse. If it doesn't,
   `FUN_101d0950` fails and the cache reset is skipped (safe failure).
5. **No new patches needed**: `FUN_101d0950` (offset 67) and `FUN_101cf5b0`
   (offset 36) are already patched. `FUN_101cfce0` (offset 40) is skipped if
   `FUN_101d0950` succeeds.

### 7.4 Risks

1. **Module element reprocessing**: The loop calls `FUN_101a4f90` for each
   dependency, which is a mini save/compile cycle. Unconfirmed if this is
   idempotent for all component types.
2. **Performance**: The loop + cmd 0x0F calls could be slow for programs with
   many dependencies.
3. **Function (category 4)**: `_VerifyModule_New@20` skips `FUN_101a8620` for
   Function types, but `FUN_101a8620` itself does NOT check for category 4.
   Calling it directly with a Function ProgId should work, but this is
   unconfirmed.

---

## 8. Evidence Index

| Function | RVA | Verification |
|----------|-----|-------------|
| `FUN_101a8620` | `0x1A8620` | Decompiled — __fastcall, 3 params, GetWarehouse(param_3), cache reset at end. Module element loop is NOT a no-op. |
| `FUN_101c09f0` | `0x1C09F0` | Decompiled — __thiscall, type code 0xC → [module+0x1DE], CSingleLock, cmd 0x4E dispatch. All claims confirmed. |
| `_GetMasterRecByType@16` | `0x1A0460` | Decompiled — ordinal 510, GetWarehouse(NULL), 0x10A-byte records, __stdcall. All claims confirmed. |
| `FUN_101d15c0` | `0x1D15C0` | Disassembled — INT3 at offset 84 (0x54). Byte pattern unique (1 match). Patch CC→90 confirmed. |
| `FUN_101a9650` | `0x1A9650` | Decompiled — NULL→CSIDL path, non-NULL→exact path, creates new warehouse if no match. Path mismatch confirmed. |
| `FUN_101a4f90` | `0x1A4F90` | Decompiled — mini save/compile cycle: GetWarehouse(NULL), cmd 0x0F, compile steps, cleanup. NOT a no-op. |
| `FUN_101e5970` | `0x1E5970` | Decompiled — returns [param_1+0xC] (element count from program object). |
| `FUN_101e12f0` | `0x1E12F0` | Decompiled — getter: retrieves element type code by index. No mutation. |
| `FUN_101e12a0` | `0x1E12A0` | Decompiled — getter: retrieves element name string by index. No mutation. |
| `FUN_101e1150` | `0x1E1150` | Decompiled — resolves element by type+name, releases COM object. Lookup-and-release. |
| `FUN_101a8750` | `0x1A8750` | Decompiled — GetWarehouse(NULL), cmd 0x0F variant, CELPrograms init. Returns program object. |
| `DAT_1023aaac` | `0x23AAAC` | Xref analysis — "verification in progress" flag. Set to 1 by _VerifyModuleEX@28, restored after. Guard confirmed. |
| `_VerifyModule_New@20` | `0x169630` | Disassembled — calls FUN_101a8620 with 50-space string for non-Function types. Caller-cleanup (ADD ESP,4) confirmed. |
| `FUN_10159ae0` | `0x159AE0` | Disassembled — second caller of FUN_101a8620, also uses 50-space string, also caller-cleanup. |

---

> **This document is research-only.** No production source, signature files, or
> tests were modified during this investigation. All verification was performed
> via Ghidra decompilation of orlib20.dll.
