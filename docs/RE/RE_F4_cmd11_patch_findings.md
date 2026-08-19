# RE Findings: Warehouse Command 0x11 Thread-Check INT3 Patch Site

**Date:** 2026-08-19
**Analyst:** RE subagent (Ghidra MCP)
**Target:** orlib20.dll v9.50.01.3344 (SHA-256 `318DB24A...`, PE timestamp `0x5C01735E`)
**Ghidra project:** `tsdev.exe` (program `/orlib20.dll`, image base `0x10000000`)
**Purpose:** Locate the INT3 (`0xCC`) thread-check patch site in the warehouse command-0x11
dispatch path (the source-text load path used by `_LoadTextFileEX@16`), mirroring the
proven NOP technique already applied for commands 0x0F, 0x1E, and 0x17.

---

## 1. Warehouse Command Dispatcher

### Architecture (event-driven command dispatch)

The orlib20 warehouse uses an **event-driven command dispatch** architecture, not a
simple switch-table call:

1. **Dispatch helpers** (one per command): Each helper acquires a mutex, performs a
   thread-ID check (INT3 on mismatch), writes the command byte to `[this+4]`, signals
   a "command-ready" event at `[this+0x6E]`, then waits on a "completion" event at
   `[this+0x6A]`.

2. **Worker thread dispatcher** (`FUN_101d2150`, RVA `0x1D2150`, body
   `0x101d2150`–`0x101d367e` — 5422 bytes): This is the CELWHouse worker thread
   procedure. It waits on the command-ready event, reads the command byte from
   `[this+4]`, and dispatches via a `switch` statement. The decompiled switch uses
   `param_1->AddRef` (Ghidra's field naming for `param_1[1]`, i.e. offset +4) as the
   switch expression — this IS the command byte set by the dispatch helpers.

### How command 0x11 is reached

The command-0x11 dispatch helper is **`FUN_101d0e80`** (RVA `0x1D0E80`). It explicitly
sets the command byte:

```c
*(undefined4 *)(param_1 + 4) = 0x11;   // at 0x101d0ebd: C7 47 04 11 00 00 00
```

**Call chain from `_LoadTextFileEX@16` (confirmed via Ghidra decompilation + xrefs):**

```
_LoadTextFileEX@16  (export, RVA 0x1081A0, addr 0x101081a0)
  └─ FUN_101a0f90   (inner logic: GetWarehouse → TypeConvert → module lookup)
       └─ FUN_101d0e80  (command-0x11 dispatch helper, addr 0x101d0e80)
            └─ INT3 thread check at 0x101d0eb9
```

**Cross-references to `FUN_101d0e80`** (2 callers, both confirmed via `get_xrefs_to`):
- `FUN_101a0f90` at `0x101a0fee` — the `_LoadTextFileEX@16` source-text load path
- `FUN_101a0f00` at `0x101a0f25` — a related load path (likely `_SaveTextEX@16` or
  similar)

**`FUN_101a0f90` decompiled excerpt** (confirming the call to `FUN_101d0e80`):
```c
_Src = (void *)FUN_101d0e80(local_14, param_4, &local_8);
// ... GlobalAlloc + memcpy/decryption of source text ...
```

**`_LoadTextFileEX@16` decompiled** (confirming it calls `FUN_101a0f90`):
```c
void _LoadTextFileEX_16(param_1, param_2, param_3, param_4) {
    FUN_101a0f90(DAT_1023aab8, param_1, param_2, param_3, param_4);
}
```

**Conclusion:** Command 0x11 is a real case in the dispatcher. `FUN_101d0e80` is the
command-0x11 dispatch helper, reached from `_LoadTextFileEX@16` → `FUN_101a0f90`.

---

## 2. INT3 Thread-Check Inside the Command-0x11 Path

### Containing function

| Field | Value |
|-------|-------|
| Ghidra symbol | `FUN_101d0e80` |
| Entry address | `0x101d0e80` |
| Entry RVA | `0x1D0E80` (image base `0x10000000`) |
| INT3 address | `0x101d0eb9` |
| INT3 byte offset from entry | `0x39` (57 decimal) |
| Calling convention | `__thiscall` (ECX = `this` pointer, moved to EDI) |

### Thread-check idiom (identical to commands 0x0F / 0x1E / 0x17)

Decompiled `FUN_101d0e80` (excerpt):
```c
WaitForSingleObject(*(HANDLE *)(param_1 + 0x72), 0xffffffff);  // acquire mutex
DVar4 = GetCurrentThreadId();
if (DVar4 != *(DWORD *)(param_1 + 0x76)) {                     // thread check
    pcVar2 = (code *)swi(3);                                    // INT3 (0xCC)
    pvVar5 = (void *)(*pcVar2)();
    return pvVar5;
}
*(undefined4 *)(param_1 + 4) = 0x11;                           // set command byte
SetEvent(*(HANDLE *)(param_1 + 0x6e));                          // signal worker
WaitForSingleObject(*(HANDLE *)(param_1 + 0x6a), 0xffffffff);  // wait for completion
ReleaseMutex(*(HANDLE *)(param_1 + 0x72));                      // release mutex
// ... SafeArray access + copy + decrypt ...
```

**Idiom comparison with the 3 existing patches:**

| Function | Command | IAT: WaitForSingleObject | IAT: GetCurrentThreadId | IAT: SetEvent | Thread check |
|----------|---------|--------------------------|-------------------------|---------------|--------------|
| `FUN_101d0950` | 0x0F | `0x101f958c` | `0x101f9620` | `0x101f9688` | `CMP [this+0x76]; JZ+1; INT3` |
| `FUN_101cf5b0` | 0x1E | `0x101f958c` | `0x101f9620` | `0x101f9688` | `CMP [this+0x76]; JZ+6; CMP cmd,0x1E; JZ+1; INT3` |
| `FUN_101d1fa0` | 0x17 | `0x101f958c` | `0x101f9620` | `0x101f9688` | `CMP [this+0x76]; JZ+1; INT3` |
| **`FUN_101d0e80`** | **0x11** | **`0x101f958c`** | **`0x101f9620`** | **`0x101f9688`** | **`CMP [this+0x76]; JZ+1; INT3`** |

All four use the **same IAT entries** and the **same `CMP EAX,[reg+0x76]; JZ+1; INT3`
pattern**. The only structural difference is that `FUN_101d0e80` uses `EDI` as the
`this` pointer (moved from ECX at entry) and loads `WaitForSingleObject` into `EBX`
(via `MOV EBX,[0x101f958c]`) then `CALL EBX`, whereas `FUN_101d1fa0` uses `ESI`
directly and calls through the IAT. The thread-check logic is identical.

### Disassembly of `FUN_101d0e80` (entry through INT3 + command set)

```
101d0e80  PUSH EBP                          ; 55
101d0e81  MOV EBP,ESP                       ; 8B EC
101d0e83  PUSH ECX                          ; 51
101d0e84  MOV EAX,[EBP+0x8]                 ; 8B 45 08        (param_2 → [EDI+0x12])
101d0e87  PUSH EBX                          ; 53
101d0e88  MOV EBX,[0x101f958c]              ; 8B 1D 8C 95 1F 10  (WaitForSingleObject)
101d0e8e  PUSH ESI                          ; 56
101d0e8f  MOV ESI,[EBP+0x10]                ; 8B 75 10        (param_4 → output size ptr)
101d0e92  PUSH EDI                          ; 57
101d0e93  MOV EDI,ECX                       ; 8B F9           (this → EDI)
101d0e95  PUSH -0x1                         ; 6A FF           (INFINITE timeout)
101d0e97  MOV dword ptr [ESI],0x0           ; C7 06 00 00 00 00
101d0e9d  MOV [EBP-0x4],EDI                 ; 89 7D FC        (save this)
101d0ea0  PUSH [EDI+0x72]                   ; FF 77 72        (mutex handle)
101d0ea3  MOV [EDI+0x12],EAX                ; 89 47 12
101d0ea6  MOV EAX,[EBP+0xc]                 ; 8B 45 0C        (param_3 → [EDI+0x16])
101d0ea9  MOV [EDI+0x16],EAX                ; 89 47 16
101d0eac  CALL EBX                          ; FF D3           (WaitForSingleObject)
101d0eae  CALL [0x101f9620]                 ; FF 15 20 96 1F 10  (GetCurrentThreadId)
101d0eb4  CMP EAX,[EDI+0x76]                ; 3B 47 76        (thread check)
101d0eb7  JZ 0x101d0eba                     ; 74 01
101d0eb9  INT3                              ; CC              ← PATCH THIS BYTE (offset 0x39)
101d0eba  PUSH [EDI+0x6e]                   ; FF 77 6E        (command-ready event)
101d0ebd  MOV dword ptr [EDI+0x4],0x11      ; C7 47 04 11 00 00 00  (COMMAND 0x11)
101d0ec4  CALL [0x101f9688]                 ; FF 15 88 96 1F 10  (SetEvent)
...
```

### Raw byte dump (80 bytes from function entry)

```
Address: 0x101d0e80, 80 bytes:

55 8B EC 51 8B 45 08 53 8B 1D 8C 95 1F 10 56 8B   ; offset  0-15
75 10 57 8B F9 6A FF C7 06 00 00 00 00 89 7D FC   ; offset 16-31
FF 77 72 89 47 12 8B 45 0C 89 47 16 FF D3 FF 15   ; offset 32-47
20 96 1F 10 3B 47 76 74 01 CC FF 77 6E C7 47 04   ; offset 48-63
11 00 00 00 FF 15 88 96 1F 10 6A FF FF 77 6A FF   ; offset 64-79
```

**Patchable byte marked:** `CC` at offset **57** (0x39) from function entry — address
`0x101d0eb9`. This is the INT3 instruction. It is to be replaced with `0x90` (NOP).

---

## 3. Proposed Signature Entry

### Naming convention

The existing 3 thread-check patch signatures follow the convention
`FUN_<hexaddr>_thread_check_INT3`:

| Existing name | Command | Referenced in `tsdev_lib.cpp` as |
|---------------|---------|----------------------------------|
| `FUN_101d0950_thread_check_INT3` | 0x0F | `m_resolver->applyPatch("FUN_101d0950_thread_check_INT3")` |
| `FUN_101cf5b0_thread_check_INT3` | 0x1E | `m_resolver->applyPatch("FUN_101cf5b0_thread_check_INT3")` |
| `FUN_101d1fa0_thread_check_INT3` | 0x17 | `m_resolver->applyPatch("FUN_101d1fa0_thread_check_INT3")` |

**New name (follows convention exactly):**
```
FUN_101d0e80_thread_check_INT3
```

**Reference string for `tsdev_lib.cpp`** (to be used in `applyThreadCheckPatches()`):
```cpp
m_resolver->applyPatch("FUN_101d0e80_thread_check_INT3");
```

**Resolver method** (to be added to `orlib_resolver.cpp` / `.h`, mirroring
`resolveThreadCheck1/2/3`):
```cpp
PatchSiteDescriptor OrlibResolver::resolveThreadCheck4() {
    return resolvePatchSite("FUN_101d0e80_thread_check_INT3");
}
```

### Proposed JSON entry (ready to paste into `signatures/v9.50.01.3344.json`)

```json
    {
      "name": "FUN_101d0e80_thread_check_INT3",
      "targetType": "patch_site",
      "callingConvention": "thiscall",
      "expectedRva": "0x1D0E80",
      "hexPattern": "55 8B EC 51 8B 45 08 53 8B 1D ?? ?? ?? ?? 56 8B 75 10 57 8B F9 6A FF C7 06 00 00 00 00 89 7D FC FF 77 72 89 47 12 8B 45 0C 89 47 16 FF D3 FF 15 ?? ?? ?? ?? 3B 47 76 74 01 CC FF 77 6E C7 47 04 11 00 00 00 FF 15 ?? ?? ?? ??",
      "length": 74,
      "expectedMatchCount": 1,
      "patch": {
        "offset": 57,
        "originalBytes": "CC",
        "replacementBytes": "90",
        "originalBytesMask": "FF",
        "description": "NOP thread-check INT3 in command 0x11 (source-text load) dispatch helper"
      },
      "containingFunction": "FUN_101d0e80",
      "provenance": {
        "author": "RE-F4",
        "date": "2026-08-19",
        "source": "Ghidra disassembly",
        "reference": "docs/RE/RE_F4_cmd11_patch_findings.md"
      }
    }
```

### Pattern construction rationale

The 74-byte pattern spans from function entry through the SetEvent call that follows
the command-byte store. Three IAT reference groups are wildcarded (`?? ?? ?? ??`):

| Offset | Wildcarded bytes | IAT address | API |
|--------|-----------------|-------------|-----|
| 10–13 | `8C 95 1F 10` | `0x101f958c` | `WaitForSingleObject` (loaded into EBX) |
| 48–51 | `20 96 1F 10` | `0x101f9620` | `GetCurrentThreadId` |
| 70–73 | `88 96 1F 10` | `0x101f9688` | `SetEvent` |

All other bytes are fixed (non-wildcarded). The `CC` at offset 57 is the patchable
byte (validated as `originalBytes: "CC"` with `originalBytesMask: "FF"`).

---

## 4. Uniqueness Verification

Three independent byte-pattern searches in orlib20.dll's `.text` section confirm the
pattern matches **exactly once**:

| Search pattern | Matches | Address |
|----------------|---------|---------|
| `55 8B EC 51 8B 45 08 53 8B 1D` (prologue, 10 bytes) | **1** | `0x101d0e80` (function entry) |
| `C7 47 04 11 00 00 00` (command-0x11 store, 7 bytes) | **1** | `0x101d0ebd` |
| `3B 47 76 74 01 CC FF 77 6E C7 47 04 11 00 00 00` (INT3 + command context, 16 bytes) | **1** | `0x101d0eb4` |

The full 74-byte pattern (with wildcards on IAT refs) is a superset of all three
verified sub-patterns, so it is guaranteed to match exactly once. The prologue
`55 8B EC 51 8B 45 08 53 8B 1D` is distinctive (PUSH EBP; MOV EBP,ESP; PUSH ECX;
MOV EAX,[EBP+8]; PUSH EBX; MOV EBX,[IAT]) and does not appear elsewhere in the
`.text` section.

**Uniqueness approach matches existing signatures:** The existing 3 thread-check
signatures use a similar strategy — a distinctive prologue + the command-byte store +
the INT3, with IAT references wildcarded. The command byte (`0x11` in
`C7 47 04 11 00 00 00`) is itself a strong uniqueness marker, as confirmed by the
single-match search.

---

## 5. Caveats / Uncertainties

1. **No code changes made (READ-ONLY):** Per constraints, no project source files,
   signatures JSON, or sigcodegen.py were modified. Only this findings document was
   created.

2. **`FUN_101d0e80` has a more complex prologue** than the other 3 dispatch helpers:
   it saves EBX/ESI/EDI and loads `WaitForSingleObject` into EBX (rather than calling
   through the IAT directly). This results in a longer signature (74 bytes vs 36–57
   for the existing 3). The longer pattern is still unique and follows the same
   wildcard policy (only IAT address bytes are wildcarded).

3. **`FUN_101d0e80` has a second caller** (`FUN_101a0f00` at `0x101a0f25`) in addition
   to the `_LoadTextFileEX@16` path (`FUN_101a0f90`). Both callers use the same
   command-0x11 dispatch, so the INT3 patch benefits both paths. The second caller
   was not fully investigated (out of scope — the task is to find the patch site, not
   enumerate all callers).

4. **Parameter order of `_LoadTextFileEX@16`** was NOT investigated (per constraints
   — a separate subagent owns that). The call chain
   `_LoadTextFileEX@16` → `FUN_101a0f90` → `FUN_101d0e80` was confirmed only to
   establish that command 0x11 is the source-text load path.

5. **Worker thread dispatcher (`FUN_101d2150`)** was identified but not fully
   decompiled (5422 bytes, output truncated). The switch on `[this+4]` (command byte)
   was confirmed via the decompiled `switch(param_1->AddRef)` statement. Individual
   case-0x11 handler code inside the worker thread was not examined — it is not
   needed for the patch (the INT3 is in the dispatch helper, not the worker thread).

6. **`m_threadCheckRvas` / `m_threadCheckOriginalBytes` arrays** in `tsdev_lib.h` are
   currently sized `[3]`. Adding a 4th patch requires expanding these to `[4]` and
   adding the 4th resolve+apply block in `applyThreadCheckPatches()`. This is an
   implementation concern for the parent agent, not a finding uncertainty.

---

## 6. Summary

| Field | Value |
|-------|-------|
| Containing function | `FUN_101d0e80` |
| Entry RVA | `0x1D0E80` |
| INT3 address | `0x101d0eb9` |
| INT3 byte offset from entry | `0x39` (57) |
| Patch (original → replacement) | `0xCC` → `0x90` (NOP) |
| Signature name | `FUN_101d0e80_thread_check_INT3` |
| Pattern length | 74 bytes |
| Uniqueness | 1 match (confirmed via 3 independent sub-pattern searches) |
| Command | 0x11 (source-text load, reached from `_LoadTextFileEX@16`) |
| Idiom match | Identical to existing 0x0F / 0x1E / 0x17 patches (same IAT, same CMP/JZ/INT3) |
