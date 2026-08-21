# RE Findings: F5 Name-Getter Thread-Check INT3 Patch Analysis

> ## ERRATUM — NAME-GETTER CONCLUSIONS BELOW ARE DISPROVEN (SUPERSEDED)
>
> **This analysis concluded that `FUN_101c2e90` (called here the "verbatim name getter, non-server variant") and `FUN_101a42d0` yield the verbatim stored item name, and recommends calling `FUN_101a42d0` directly so that "the result is the name pointer". This was DISPROVEN by runtime tracing (Frida, 2026-08-20): in non-server mode both functions return the ProgId, not the verbatim name.** `FUN_101c2e90` is a **ProgId getter**, and `FUN_101a42d0`'s non-server path returns that ProgId — it cannot yield the verbatim name.
>
> **Correct, implemented-and-verified approach:** call `FUN_101d05e0` (BST search, `__thiscall`, ECX=warehouse, stack=(typeFilter, name, *outNode), RET 0xC, returns bool + node pointer), then call `FUN_101c36d0` (verbatim-name getter, `__fastcall`, ECX=node, returns `char*` to the verbatim name stored at warehouse node offset +0x1D), with the thread-check INT3 inside `FUN_101c36d0` at offset 0x6A (RVA `0x1C373A`) NOP'd. Authoritative details: `docs\RE\RE_F5_bst_node_structure.md` and `docs\SIGNATURE_CATALOG.md` (signatures 23–25).
>
> **Readers: treat the body's name-getter conclusions and its §8 recommendations as SUPERSEDED/historical. Do NOT implement them.** The body text below is intentionally left unmodified as a historical record of the incorrect static-analysis reasoning.

**Date:** 2026-08-19
**Analyst:** RE subagent (Ghidra MCP)
**Target:** orlib20.dll v9.50.01.3344 (SHA-256 `318DB24A...`, PE timestamp `0x5C01735E`)
**Ghidra project:** `tsdev.exe` (program `/orlib20.dll`, image base `0x10000000`)
**Purpose:** Determine whether `FUN_101a42d0` (RVA 0x1a42d0) and `FUN_101c36d0`
(RVA 0x1c36d0) have thread-check INT3 instructions that need patching before
calling them from our CLI thread, and extract signature patterns for any
functions requiring patches.

---

## Executive Summary

**Three functions need thread-check INT3 patches** to safely call
`FUN_101a42d0` from our CLI thread. `FUN_101a42d0` itself has no INT3, but
every call path through it reaches one or more thread-check INT3s in
subordinate command-dispatch helpers. The already-patched `FUN_101cf5b0`
(command 0x1E) is also on the call path but is safe.

| Function | RVA | INT3 offset | INT3 address | Needs patch? |
|----------|-----|-------------|--------------|--------------|
| `FUN_101a42d0` | 0x1a42d0 | — | — | No (delegates to helpers) |
| `FUN_101c36d0` | 0x1c36d0 | — | — | No (delegates to `FUN_101cfce0`) |
| `FUN_101d04f0` | 0x1d04f0 | — | — | No (delegates to helpers) |
| **`FUN_101cfce0`** | **0x1cfce0** | **0x28** | **0x101cfd08** | **YES** |
| **`FUN_101d15c0`** | **0x1d15c0** | **0x54** | **0x101d1614** | **YES** |
| **`FUN_101d18c0`** | **0x1d18c0** | **0x126** | **0x101d19e6** | **YES** |
| `FUN_101cf5b0` | 0x1cf5b0 | 0x24 | 0x101cf5d4 | No (already patched) |

**Recommended approach:** Call `FUN_101a42d0` directly after patching the
three new INT3 sites. See §7 for details.

---

## 1. FUN_101a42d0 (RVA 0x1a42d0) — Thread-Check Analysis

### 1.1 Decompile result

```c
undefined4 __fastcall
FUN_101a42d0(char *param_1, undefined4 param_2, undefined4 param_3, undefined4 *param_4)
{
    // strlen(param_1) — if empty, return 0xFFFFFFFF
    // ...
    iVar2 = FUN_101a9650(0);           // get/init warehouse
    if (iVar2 != 0) {
        if (DAT_1023aa8f == '\x01') {  // SERVER MODE
            iVar2 = FUN_101d04f0(local_c, param_3, &local_8);  // BST search
            if (iVar2 == 1) {
                pcVar3 = (char *)FUN_101c36d0();                // get verbatim name
                *param_4 = pcVar3;                              // store output
                iVar2 = _strnicmp(pcVar3, local_10, 8);        // verify match
                if (iVar2 == 0) uVar4 = param_3;               // success
                FUN_1000de70();                                 // release ref
            }
        } else {                    // NON-SERVER MODE
            FUN_101d43a0(local_c, local_4c);                    // build type filter
            iVar2 = FUN_101d05e0(local_4c, param_1, &local_8); // BST search
            if (iVar2 == 1) {
                uVar4 = FUN_101c2e90();                         // get verbatim name
                FUN_1000de70();                                 // release ref
                return uVar4;
            }
        }
    }
    return uVar4;  // 0xFFFFFFFF if not found
}
```

### 1.2 INT3 in FUN_101a42d0 itself

**None.** The function body (0x101a42d0–0x101a439c, 82 instructions) contains
no INT3 instructions. It is a pure dispatcher that delegates to helpers.

### 1.3 Call graph and thread-check reachability

```
FUN_101a42d0
├── FUN_101a9650(0)           — warehouse init, NO thread check
├── [DAT_1023aa8f == 1] SERVER MODE:
│   ├── FUN_101d04f0          — BST search wrapper, NO thread check itself
│   │   ├── [warehouse+0xc2 == 1]:
│   │   │   └── FUN_101e3990  — BST search
│   │   │       ├── FUN_101d15c0  ← INT3 at 0x101d1614 (thread check)
│   │   │       └── FUN_101c36d0  → FUN_101cfce0 ← INT3 at 0x101cfd08
│   │   └── [warehouse+0xc2 != 1]:
│   │       └── FUN_101d15c0  ← INT3 at 0x101d1614 (thread check)
│   ├── FUN_101c36d0          — verbatim name getter
│   │   ├── FUN_101c82b0      — warehouse bind (INT3 is null-check, NOT thread check)
│   │   ├── FUN_101cfce0      ← INT3 at 0x101cfd08 (thread check)
│   │   └── FUN_101cf5b0(0x34) — ALREADY PATCHED (command 0x1E)
│   └── FUN_1000de70          — ref release, NO thread check
└── [DAT_1023aa8f != 1] NON-SERVER MODE:
    ├── FUN_101d43a0          — type filter builder, NO thread check
    ├── FUN_101d05e0          — BST search wrapper, NO thread check itself
    │   ├── [warehouse+0xc2 == 1]:
    │   │   └── FUN_101e3db0  — BST search
    │   │       ├── FUN_101d18c0  ← INT3 at 0x101d19e6 (thread check)
    │   │       └── FUN_101c36d0  → FUN_101cfce0 ← INT3 at 0x101cfd08
    │   └── [warehouse+0xc2 != 1]:
    │       └── FUN_101d18c0  ← INT3 at 0x101d19e6 (thread check)
    ├── FUN_101c2e90          — verbatim name getter (non-server variant)
    │   ├── FUN_101c82b0      — warehouse bind (INT3 is null-check, NOT thread check)
    │   ├── FUN_101cfce0      ← INT3 at 0x101cfd08 (thread check)
    │   └── FUN_101cf5b0(0x33) — ALREADY PATCHED (command 0x1E)
    └── FUN_1000de70          — ref release, NO thread check
```

**Conclusion:** Every path through `FUN_101a42d0` reaches at least one
thread-check INT3. The three unpatched INT3 sites that need patching are:
1. `FUN_101cfce0` at offset 0x28 (address 0x101cfd08)
2. `FUN_101d15c0` at offset 0x54 (address 0x101d1614)
3. `FUN_101d18c0` at offset 0x126 (address 0x101d19e6)

---

## 2. FUN_101c36d0 (RVA 0x1c36d0) — Thread-Check Analysis

### 2.1 Function overview

`FUN_101c36d0` is the verbatim-name getter used in server mode. It is a
`__fastcall` function (ECX = data node pointer from BST search). It lazily
initializes the stored name by dispatching a command to the warehouse worker
thread via `FUN_101cfce0`, then caches the result.

### 2.2 INT3 in FUN_101c36d0 itself

There is one INT3 at address 0x101c373a (offset 0x6a from entry):

```asm
101c3733  MOV [EDI+0x10],EAX        ; store FUN_101cfce0 result
101c3736  TEST EAX,EAX
101c3738  JNZ 0x101c373b
101c373a  INT3                      ; ← null-result assertion (NOT a thread check)
```

**This is NOT a thread-check INT3.** It fires only if `FUN_101cfce0` returns 0
(null), indicating the command dispatch failed. Once `FUN_101cfce0`'s
thread-check is patched, this assertion will not fire (the command will
succeed and return a non-zero value). **No patch needed for this INT3.**

### 2.3 Thread-check INT3 in callees

`FUN_101c36d0` calls:
- `FUN_101c82b0` — has an INT3 (swi(3)) but it's a **null-check** on the
  warehouse pointer (`if (warehouse == 0) INT3`), not a thread check. Safe.
- **`FUN_101cfce0`** — has a **thread-check INT3 at offset 0x28** (address
  0x101cfd08). **NEEDS PATCHING.**
- `FUN_101cf5b0(0x34)` — command 0x1E dispatch helper. **Already patched**
  (thread-check at offset 0x24, address 0x101cf5d4).

---

## 3. FUN_101d04f0 (RVA 0x1d04f0) — Thread-Check Analysis

### 3.1 Function overview

`FUN_101d04f0` is the BST search wrapper used in server mode. It is a
`__thiscall` function (ECX = warehouse, stack params = type, searchName,
outNodePtr). It branches on `[warehouse+0xc2]`:

```c
bool __thiscall FUN_101d04f0(int warehouse, undefined4 type, undefined4 name, int *outNode)
{
    if (*(int *)(warehouse + 0xc2) == 1) {
        FUN_101a9460(type, name);           // lazy init BST
        iVar1 = FUN_101e3990(type, name);   // BST search (in-memory)
        *outNode = iVar1;
        return *outNode != 0;
    }
    FUN_101d15c0(type, name, outNode);      // BST search (command dispatch)
    return *outNode != 0;
}
```

### 3.2 INT3 in FUN_101d04f0 itself

**None.** The function body (0x101d04f0–0x101d0538, 29 instructions) contains
no INT3 instructions.

### 3.3 Thread-check INT3 in callees

- `[warehouse+0xc2] == 1` path: `FUN_101e3990` → calls `FUN_101d15c0`
  (INT3 at 0x101d1614) and `FUN_101c36d0` (→ `FUN_101cfce0` INT3 at 0x101cfd08)
- `[warehouse+0xc2] != 1` path: `FUN_101d15c0` directly (INT3 at 0x101d1614)

### 3.4 Comparison with FUN_101d05e0 (FindExistPos's BST search)

`FUN_101d05e0` (RVA 0x1d05e0) is the non-server-mode equivalent. It has the
same structure but calls `FUN_101e3db0` and `FUN_101d18c0` instead of
`FUN_101e3990` and `FUN_101d15c0`. Both `FUN_101d05e0` and `FUN_101d04f0`
are structurally identical wrappers that differ only in which BST search
helpers they dispatch to. `FUN_101d05e0` is the function already used by
our `FindExistPos` implementation.

---

## 4. Thread-Check INT3 Sites Requiring Patches

### 4.1 FUN_101cfce0 (RVA 0x1cfce0) — Command Dispatch Worker

This is the core command-dispatch worker used by both `FUN_101c36d0` and
`FUN_101c2e90` (and many other functions). It dispatches a command to the
warehouse worker thread.

| Field | Value |
|-------|-------|
| Ghidra symbol | `FUN_101cfce0` |
| Entry address | `0x101cfce0` |
| Entry RVA | `0x1CFCE0` |
| INT3 address | `0x101cfd08` |
| INT3 byte offset from entry | `0x28` (40 decimal) |
| Calling convention | `__thiscall` (ECX = `this`/warehouse, stack = param_2, param_3) |
| Command byte | `0x0D` (set at `[this+4]`) |

**Thread-check idiom (disassembly):**
```asm
101cfcf7  CALL [0x101f958c]          ; WaitForSingleObject(mutex)
101cfcfd  CALL [0x101f9620]          ; GetCurrentThreadId()
101cfd03  CMP EAX,[ESI+0x76]         ; compare thread ID
101cfd06  JZ  +0x01                  ; skip INT3 if match
101cfd08  INT3                       ; ← THREAD CHECK (0xCC)
101cfd09  PUSH [ESI+0x6e]            ; SetEvent (command ready)
101cfd0c  MOV dword ptr [ESI+4],0xD  ; command byte = 0x0D
101cfd13  CALL [0x101f9688]          ; SetEvent
```

**Patch:** NOP the INT3 at 0x101cfd08 (1 byte: `0xCC` → `0x90`).

### 4.2 FUN_101d15c0 (RVA 0x1d15c0) — BST Search via Command Dispatch

This is the BST search helper that dispatches a command to the worker thread
(non-in-memory path). Used by `FUN_101d04f0` (server mode) and `FUN_101e3990`.

| Field | Value |
|-------|-------|
| Ghidra symbol | `FUN_101d15c0` |
| Entry address | `0x101d15c0` |
| Entry RVA | `0x1D15C0` |
| INT3 address | `0x101d1614` |
| INT3 byte offset from entry | `0x54` (84 decimal) |
| Calling convention | `__thiscall` (ECX = `this`/warehouse, stack = type, name, outNode) |
| Command byte | `0x0D` (set at `[this+4]`) |

**Thread-check idiom (disassembly):**
```asm
101d1603  CALL [0x101f958c]          ; WaitForSingleObject(mutex)
101d1609  CALL [0x101f9620]          ; GetCurrentThreadId()
101d160f  CMP EAX,[ESI+0x76]         ; compare thread ID
101d1612  JZ  +0x01                  ; skip INT3 if match
101d1614  INT3                       ; ← THREAD CHECK (0xCC)
101d1615  PUSH [ESI+0x6e]            ; SetEvent (command ready)
101d1618  MOV dword ptr [ESI+4],0xD  ; command byte = 0x0D
101d161f  CALL [0x101f9688]          ; SetEvent
```

**Patch:** NOP the INT3 at 0x101d1614 (1 byte: `0xCC` → `0x90`).

### 4.3 FUN_101d18c0 (RVA 0x1d18c0) — BST Search via Command Dispatch (Non-Server Variant)

This is the BST search helper used by `FUN_101d05e0` (non-server mode) and
`FUN_101e3db0`. It is structurally similar to `FUN_101d15c0` but uses command
byte `0x10` instead of `0x0D` and has additional SafeArray setup.

| Field | Value |
|-------|-------|
| Ghidra symbol | `FUN_101d18c0` |
| Entry address | `0x101d18c0` |
| Entry RVA | `0x1D18C0` |
| INT3 address | `0x101d19e6` |
| INT3 byte offset from entry | `0x126` (294 decimal) |
| Calling convention | `__thiscall` (ECX = `this`/warehouse, stack = type, name, outNode) |
| Command byte | `0x10` (set at `[this+4]`) |

**Thread-check idiom (disassembly):**
```asm
101d19d9  CALL EBX                   ; WaitForSingleObject(mutex)
101d19db  CALL [0x101f9620]          ; GetCurrentThreadId()
101d19e1  CMP EAX,[ESI+0x76]         ; compare thread ID
101d19e4  JZ  +0x01                  ; skip INT3 if match
101d19e6  INT3                       ; ← THREAD CHECK (0xCC)
101d19e7  PUSH [ESI+0x6e]            ; SetEvent (command ready)
101d19ea  MOV dword ptr [ESI+4],0x10 ; command byte = 0x10
101d19f1  CALL [0x101f9688]          ; SetEvent
```

**Patch:** NOP the INT3 at 0x101d19e6 (1 byte: `0xCC` → `0x90`).

---

## 5. Signature Patterns

Each signature is verified to match exactly once in the orlib20.dll binary
via Ghidra's `search_byte_patterns` tool.

### 5.1 FUN_101cfce0 — Signature (command 0x0D, unique)

**Pattern (hex, 42 bytes):**
```
74 01 CC FF 76 6E C7 46 04 0D 00 00 00 FF 15 88 96 1F 10 6A FF FF 76 6A FF 15 8C 95 1F 10 FF 76 72 FF 15 90 95 1F 10 8B 46 5E C7 46 5E 00 00 00 00 5E 5D C2 08 00
```

**Match count:** 1 (at `0x101cfd06`, i.e. 2 bytes before the INT3 at `0x101cfd08`)

**Rationale:** This pattern starts at the `JZ +1` before the INT3 and includes
the full command-dispatch tail (command byte 0x0D, IAT calls, result field
clear `C7 46 5E 00 00 00 00`, and epilogue `5E 5D C2 08 00`). The trailing
epilogue distinguishes it from `FUN_101d0660` which has the same command byte
but a different tail.

**INT3 offset within pattern:** byte 2 (the `CC`)

### 5.2 FUN_101d15c0 — Signature (command 0x0D, unique)

**Pattern (hex, 32 bytes):**
```
C7 03 00 00 00 00 FF 76 72 89 46 12 8B 45 0C 89 46 16 FF 15 8C 95 1F 10 FF 15 20 96 1F 10 3B 46 76 74 01 CC
```

**Match count:** 1 (at `0x101d15f1`, i.e. 0x31 bytes before the INT3 at `0x101d1614`)

**Rationale:** This pattern starts from the distinctive `C7 03 00 00 00 00`
(clearing the output node to zero, unique to `FUN_101d15c0`'s prologue) and
extends through the thread-check `3B 46 76 74 01 CC`. The `C7 03` prefix
distinguishes it from `FUN_101cfce0` and other command-0x0D dispatch helpers.

**INT3 offset within pattern:** byte 35 (the `CC`)

### 5.3 FUN_101d18c0 — Signature (command 0x10, unique)

**Pattern (hex, 30 bytes):**
```
74 01 CC FF 76 6E C7 46 04 10 00 00 00 FF 15 88 96 1F 10 6A FF FF 76 6A FF D3
```

**Match count:** 1 (at `0x101d19e4`, i.e. 2 bytes before the INT3 at `0x101d19e6`)

**Rationale:** This pattern starts at the `JZ +1` before the INT3 and includes
command byte `0x10` (unique to `FUN_101d18c0`) and the `FF D3` (CALL EBX) for
the second WaitForSingleObject call. The command byte 0x10 and `FF D3`
distinguish it from all other dispatch helpers.

**INT3 offset within pattern:** byte 2 (the `CC`)

---

## 6. Calling Convention of FUN_101a42d0

### 6.1 Signature

```c
undefined4 __fastcall
FUN_101a42d0(char *searchName, int componentType, int warehouse, undefined4 *outVerbatimName);
```

### 6.2 Parameter mapping (from disassembly)

| Parameter | Register/Stack | Ghidra name | Notes |
|-----------|---------------|-------------|-------|
| `searchName` | ECX | param_1 | Null-terminated char* (the name to search for) |
| `componentType` | EDX | param_2 | int (e.g. 0=Strategy, 2=Indicator, 4=Function, 5122=TradingApp) |
| `warehouse` | [ESP+0] → [EBP+8] | param_3 | CELProgram/warehouse object pointer (from `FUN_101a9650`) |
| `outVerbatimName` | [ESP+4] → [EBP+0xc] | param_4 | `undefined4*` — receives `char*` to the verbatim stored name |

**Return:** `undefined4` — `0xFFFFFFFF` if not found; non-zero (warehouse
pointer in server mode, name pointer in non-server mode) on success.

**Stack cleanup:** `RET 0x8` (callee cleans 2 stack DWORDs = 8 bytes)

### 6.3 Disassembly verification

```asm
101a42d0  PUSH EBP
101a42d1  MOV EBP,ESP
101a42d3  SUB ESP,0x48
101a42d6  PUSH EBX
101a42d7  MOV EBX,ECX              ; EBX = searchName (param_1)
101a42d9  MOV [EBP-8],EDX          ; local_8 = componentType (param_2)
101a42dc  PUSH ESI
...
101a4316  MOV EBX,[EBP+8]          ; EBX = warehouse (param_3)
...
101a4323  CALL 0x101d04f0          ; FUN_101d04f0(componentType, warehouse, &local_8)
...
101a4330  CALL 0x101c36d0          ; FUN_101c36d0() → EAX = verbatim name
101a4335  MOV ECX,[EBP+0xc]        ; ECX = outVerbatimName (param_4)
101a433e  MOV [ECX],EAX            ; *outVerbatimName = name pointer
...
101a435e  RET 0x8                  ; clean up 2 stack params
```

### 6.4 Output name pointer

In **server mode** (`DAT_1023aa8f == 1`): the verbatim name pointer is stored
via `*param_4 = FUN_101c36d0_result`. The return value is the warehouse
pointer (non-zero = success).

In **non-server mode** (`DAT_1023aa8f != 1`): the verbatim name pointer is
returned directly from `FUN_101c2e90()`. The `param_4` output pointer is NOT
written in this path.

**Important:** Callers must check the return value for `0xFFFFFFFF` (not
found). In server mode, retrieve the name from `*outVerbatimName`. In
non-server mode, retrieve the name from the return value.

---

## 7. DAT_1023aa8f — Server Mode Flag

### 7.1 What is DAT_1023aa8f?

`DAT_1023aa8f` is a **global byte** at RVA `0x23aa8f` (address `0x1023aa8f`).

### 7.2 Cross-references

| Address | Type | Function |
|---------|------|----------|
| `0x101a430d` | READ | `FUN_101a42d0` — `CMP byte ptr [0x1023aa8f], 0x1` |
| `0x101a44d8` | WRITE | `CheckWorkAreaVersion` — `DAT_1023aa8f = (iVar1 == 0)` |

### 7.3 How it is set

`CheckWorkAreaVersion` (RVA 0x1a4490) sets the flag:

```c
undefined4 CheckWorkAreaVersion(undefined4 param_1) {
    iVar1 = FUN_101a9650(0);          // get/init warehouse
    if (iVar1 != 0) {
        // Compare work area version against "10.90.06.9020"
        // (local_14 = "10.90.06", local_c = ".902", local_8 = "0")
        iVar1 = FUN_101cf600(&local_14);   // version compare
        DAT_1023aa8f = (iVar1 == 0);        // 1 if version matches, 0 otherwise
        uVar2 = FUN_101cf600(param_1);      // compare caller's version
        return uVar2;
    }
    return 0xffffffff;
}
```

`DAT_1023aa8f = 1` when the work area version matches the expected version
string (embedded as `"10.90.06.9020"`). This is NOT the same as
`[warehouse+0xc2]` — that is a per-warehouse object field that controls
in-memory vs. command-dispatch BST search.

### 7.4 Value in our CLI

In our CLI, `CheckWorkAreaVersion` is **not called** during standard
initialization (our init path is `FUN_101a9650(0)` → warehouse init, which
does not call `CheckWorkAreaVersion`). Therefore `DAT_1023aa8f` retains its
default BSS value of **0** (uninitialized global → zero-filled).

**Consequence:** `FUN_101a42d0` will take the **non-server mode** path
(the `ELSE` branch), which calls:
- `FUN_101d43a0` (type filter builder)
- `FUN_101d05e0` (BST search — same function used by `FindExistPos`)
- `FUN_101c2e90` (verbatim name getter)

This means the thread-check INT3s hit will be:
- `FUN_101cfce0` at 0x101cfd08 (via `FUN_101c2e90`)
- `FUN_101d18c0` at 0x101d19e6 (via `FUN_101d05e0` → `FUN_101e3db0` or directly)

If we also want to support server mode (e.g. if `CheckWorkAreaVersion` gets
called in some init path), we should also patch `FUN_101d15c0` at 0x101d1614.
**Recommendation: patch all three** to be safe regardless of mode.

---

## 8. Recommended Approach

### 8.1 Call FUN_101a42d0 directly (recommended)

**Rationale:** `FUN_101a42d0` is the highest-level name getter. It handles
both server and non-server modes, performs the BST search, retrieves the
verbatim name, and even does an `_strnicmp` verification. Calling it directly
is the closest emulation of what TSDev does internally.

**Prerequisites:**
1. Patch `FUN_101cfce0` INT3 at 0x101cfd08 (1 byte NOP)
2. Patch `FUN_101d15c0` INT3 at 0x101d1614 (1 byte NOP) — for server mode safety
3. Patch `FUN_101d18c0` INT3 at 0x101d19e6 (1 byte NOP) — for non-server mode
4. `FUN_101cf5b0` is already patched (command 0x1E)

**Call from C++ (non-server mode, DAT_1023aa8f == 0):**
```cpp
// ECX = searchName (char*), EDX = componentType (int)
// Stack: warehouse (int), outVerbatimName (undefined4*)
// Returns: 0xFFFFFFFF if not found, name pointer if found (non-server mode)
typedef unsigned int (__fastcall *GetNameFn)(const char*, int, int, unsigned int*);
GetNameFn getName = (GetNameFn)(base + 0x1a42d0);
unsigned int outName = 0;
unsigned int result = getName(searchName, componentType, warehouse, &outName);
// In non-server mode: result is the name pointer (or 0xFFFFFFFF)
// In server mode: outName is the name pointer, result is warehouse (non-zero)
```

### 8.2 Alternative: Call FUN_101c36d0 after FindExistPos

This would require:
1. Get the data node from `FindExistPos` (already implemented)
2. Call `FUN_101c36d0(dataNode)` to get the verbatim name

**Prerequisites:** Same patches needed (`FUN_101cfce0` at minimum, plus
`FUN_101d15c0`/`FUN_101d18c0` if `FindExistPos` itself needs them).

**Disadvantage:** `FUN_101c36d0` only works in server mode (it's the
server-mode name getter). In non-server mode, `FUN_101c2e90` is used instead.
We would need to know which mode we're in and call the right function.
`FUN_101a42d0` handles this automatically.

### 8.3 Recommendation

**Call `FUN_101a42d0` directly.** It is the complete, self-contained name
getter that handles all modes. The three INT3 patches are simple 1-byte NOPs
with verified-unique signatures. This approach most closely emulates TSDev's
internal behavior and avoids mode-detection logic.

---

## 9. Patch Summary Table

| # | Function | RVA | INT3 address | INT3 offset | Patch | Signature match count |
|---|----------|-----|-------------|-------------|-------|-----------------------|
| 1 | `FUN_101cfce0` | 0x1cfce0 | 0x101cfd08 | 0x28 | `CC`→`90` | 1 (unique) |
| 2 | `FUN_101d15c0` | 0x1d15c0 | 0x101d1614 | 0x54 | `CC`→`90` | 1 (unique) |
| 3 | `FUN_101d18c0` | 0x1d18c0 | 0x101d19e6 | 0x126 | `CC`→`90` | 1 (unique) |
| (existing) | `FUN_101cf5b0` | 0x1cf5b0 | 0x101cf5d4 | 0x24 | Already patched | — |

All three new patches use the same proven 1-byte NOP technique (`0xCC` →
`0x90`) already applied for commands 0x0F, 0x1E, 0x11, and 0x17.
