# RE_M4_DEPLIST_ABI.md — ABI of `_GetStaticDependencyList@12` and `_DeleteDependencyList@4`

**Status:** Durable RE note (read-only Ghidra static analysis).
**Source binary:** `orlib20.dll` (Ghidra project `TSDevAnalysis`, image base `0x10000000`).
**Method:** Ghidra MCP decompilation + disassembly of the two export RVAs and their callees.
**Date:** 2026-09-26

---

## 0. Targets

| Export name | Ordinal | RVA | Loaded address | Stack cleanup |
|---|---|---|---|---|
| `_GetStaticDependencyList@12` | 531 | `0x1A8F70` | `0x101A8F70` | `RET 0xC` (12 bytes) |
| `_DeleteDependencyList@4`    | 397 | `0x1A9150` | `0x101A9150` | `RET 0x4` (4 bytes)  |

**Important aliasing finding:** Address `0x101A9150` is exported under **two** ordinals/names:
- ordinal 396 `_DeleteChangedModulesList@4`
- ordinal 397 `_DeleteDependencyList@4`

They are the **same function body** (Ghidra plate comment at `0x1a9150` lists both). So
`_DeleteDependencyList@4` and `_DeleteChangedModulesList@4` are aliases. Any ABI conclusion
about one applies to the other.

---

## 1. `_GetStaticDependencyList@12` (ordinal 531, `0x101A8F70`)

### 1.1 Calling convention — VERIFIED stdcall, 3 params (12 bytes)

**Evidence (disassembly, prologue/epilogue):**
```
101a8f70  PUSH EBP
101a8f71  MOV EBP,ESP
...
101a905b  RET 0xc              ; callee cleans 12 bytes -> stdcall, 3 x 4-byte params
```
`RET 0xC` (ret 12) is the definitive stdcall signature: the callee pops 12 bytes (= 3 × 4-byte
parameters) before returning. The `@12` decoration is therefore correct.

Parameter register/stack assignment (from the prologue reads):
- `param_1` = `[EBP + 0x8]`  → loaded into `ESI` (`MOV ESI,dword ptr [EBP + 0x8]`)
- `param_2` = `[EBP + 0xC]`  → loaded into `EDX` (`MOV EDX,dword ptr [EBP + 0xc]`)
- `param_3` = `[EBP + 0x10]` → pushed for the count helper (`PUSH dword ptr [EBP + 0x10]`)

Ghidra decompiled signature:
```c
undefined4 * _GetStaticDependencyList_12(int param_1, undefined4 param_2, undefined4 param_3)
```

### 1.2 Parameter types/meaning

| Param | Stack | C type (inferred) | Meaning (evidence-backed) |
|---|---|---|---|
| `param_1` | `EBP+8`  | `int` (pointer/flag) | **Context selector.** Stored in `iVar6`. If `== 0`: iterate the global dependency tree at `DAT_1023dccc` and accumulate entries from every node. If `!= 0`: treat it as a single object pointer and call the fill helper once with `ECX = param_1` (thiscall). Also passed as `this` (ECX) to the count helper `FUN_101a8bd0` and to the fill helper `FUN_101a8e60`. |
| `param_2` | `EBP+0xC` | `int` (undefined4) | **Header value 0.** Stored verbatim into the first int of the returned buffer: `*puVar9 = param_2;` (offset 0). Also passed as EDX (2nd fastcall arg) to the count helper `FUN_101a8bd0`. Exact semantic meaning (a "type" / "module id" / context tag) is **UNDETERMINED** from static analysis alone. |
| `param_3` | `EBP+0x10` | `int` (undefined4) | **Header value 1 / key.** Stored verbatim into the second int of the returned buffer: `puVar9[1] = param_3;` (offset 4). Passed as the stack arg to the count helper `FUN_101a8bd0(param_1, param_2, param_3)` and as the first stack arg to the fill helper `FUN_101a8e60`. Acts as the lookup key into the dependency map (the fill helper uses it to find a range via `FUN_101452f0`/`FUN_101452b0`). Exact semantic meaning is **UNDETERMINED** from static analysis alone. |

**Evidence (decompilation, key lines):**
```c
iVar6 = param_1;
iVar8 = FUN_101a8bd0(param_3);                 // count entries (param_3 as key)
lVar4 = (ulonglong)(iVar8 * 2 + 4) * 4;        // byte size = (count*2 + 4) * 4
puVar9 = operator_new__( ... lVar4 ... );       // allocate via C++ operator new
param_1 = 2;                                    // running int-count starts at 2 (after 2 header ints)
*puVar9 = param_2;                              // offset 0  = param_2
puVar9[1] = param_3;                            // offset 4  = param_3
...
if (iVar6 == 0) {                               // param_1 == 0 -> walk global tree
    piVar2 = (int *)*DAT_1023dccc;
    while (piVar2 != puVar5) {
      FUN_101a8e60(param_3, puVar9, &param_1);  // append entries, dedup, update count
      ... tree traversal (next node) ...
    }
}
else {                                          // param_1 != 0 -> single object
    FUN_101a8e60(param_3, puVar9, &param_1);
}
puVar9[param_1] = 0xffffffff;                   // terminator int 0 = -1
puVar9[param_1 + 1] = 0xffffffff;               // terminator int 1 = -1
return puVar9;
```

### 1.3 Return type

**`undefined4 *` (pointer to int / pointer to the allocated buffer).** Ghidra's decompiled return
type is `undefined4 *`. The function returns `puVar9`, the buffer allocated by `operator new`.
Confirmed by `MOV EAX,EBX` just before `RET 0xC` (EAX = buffer pointer).

### 1.4 Memory ownership

**Caller owns the returned buffer.** The buffer is allocated with C++ `operator new` (the
`operator_new__` call at `0x101A8F9C` resolves to the import at `[0x101f9df0]`). The function
returns the raw pointer and does **not** retain any reference to it. The matching free is
`_DeleteDependencyList@4` (see §2), which calls `operator delete` on the pointer. The caller is
expected to free the buffer via `_DeleteDependencyList@4`.

### 1.5 Format/structure of the returned dependency list

The buffer is a flat `int` array with three regions. Layout (all offsets in bytes, ints are 4-byte
little-endian):

```
offset  size  field            source
------  ----  ---------------  ----------------------------------------
  0     4     header[0]        = param_2   (verbatim copy of 2nd arg)
  4     4     header[1]        = param_3   (verbatim copy of 3rd arg)
  8     8*N   entries[N]       each entry = { int a; int b; }  (8 bytes)
  8+8N  4     terminator[0]    = 0xFFFFFFFF (-1)
 12+8N  4     terminator[1]    = 0xFFFFFFFF (-1)
```

**Evidence:**
- Allocation size formula: `(iVar8 * 2 + 4) * 4` bytes = `iVar8*8 + 16`. With `iVar8` = max entry
  count from `FUN_101a8bd0`, this is `N*8` (entries) + `16` bytes = 2 header ints + 2 terminator
  ints. So the buffer always has exactly 2 header ints + N entry-pairs + 2 terminator ints.
- Header write: `*puVar9 = param_2; puVar9[1] = param_3;` (offsets 0 and 4).
- The running count `param_1` (local) is initialized to `2` (i.e. the first free int-slot is
  index 2, byte offset 8) and is incremented by `2` for every appended entry inside
  `FUN_101a8e60` (`local_8 = local_8 + 2;`). So entries are written as **pairs of ints** at
  `puVar9[count]` and `puVar9[count+1]`, i.e. 8 bytes per entry.
- Terminator write: `puVar9[param_1] = 0xffffffff; puVar9[param_1 + 1] = 0xffffffff;` — two
  sentinel ints (`-1, -1`) immediately after the last entry.
- Entry semantics (from `FUN_101a8e60` body, see §1.6): each entry is a pair `{int, int}` read
  from an internal 8-byte-stride array (`*(int*)(base + local_c*8)` and
  `*(int*)(base + 4 + local_c*8)`). The pair is deduplicated against existing entries before
  appending. The exact meaning of the two ints per entry is **UNDETERMINED** from static analysis
  (likely a {type/id, value} pair — e.g. a dependency kind plus a dependency identifier — but this
  is not provable from the decompilation alone).

**Iteration contract for consumers:** Start reading entries at byte offset 8 (int index 2), in
8-byte strides. Stop when an entry equals `{0xFFFFFFFF, 0xFFFFFFFF}`. The entry count is not
stored explicitly; it is implied by the terminator position. (Equivalently: number of entries =
(terminator_int_index - 2) / 2.)

### 1.6 Fill helper `FUN_101a8e60` (callee) — confirms entry format

`FUN_101a8e60` is invoked as a **thiscall**: `this = ECX = param_1` (the original first arg of
`_GetStaticDependencyList`), with 3 stack args (`param_3` key, `puVar9` buffer, `&count`). The
decompiler renders it with extra params because of the thiscall+fastcall mix, but the call site
is unambiguous:
```
101a9033  LEA ECX,[EBP + 0x8]   ; &count (local)
101a9036  PUSH ECX              ; arg3: &count
101a9037  PUSH EBX              ; arg2: puVar9 (buffer)
101a9038  PUSH EAX              ; arg1: param_3 (key)
101a9039  MOV ECX,ESI           ; this = param_1 (original)
101a903b  CALL 0x101a8e60
101a9040  ADD ESP,0xc           ; cleanup 3 stack args (thiscall)
```

Body (key loop) — proves entries are 8-byte `{int,int}` pairs with deduplication:
```c
local_8 = *param_5;                       // current count (in ints)
do {
    iVar1 = *(int *)(iVar2 + 0x18);
    iVar3 = *(int *)(iVar1 + local_c * 8);        // entry.a  (source base, stride 8)
    iVar1 = *(int *)(iVar1 + 4 + local_c * 8);    // entry.b
    iVar4 = 0;
    if (0 < local_8 / 2) {
      do {                                         // dedup: scan existing entries
        if ((*(int *)(param_4 + iVar4 * 4) == iVar3) &&
           (*(int *)(param_4 + 4 + iVar4 * 4) == iVar1)) goto LAB_101a8f47;
        iVar4 = iVar4 + 2;
      } while (iVar4 < local_8 / 2);
    }
    *(int *)(param_4 + local_8 * 4) = iVar3;       // append entry.a
    *(int *)(param_4 + 4 + local_8 * 4) = iVar1;   // append entry.b
    local_8 = local_8 + 2;                          // count += 2 ints (one 8-byte entry)
LAB_101a8f47:
    local_c = local_c + 1;
} while (local_c < local_10);
*param_5 = local_8;                                // write back updated count
```
This confirms: (a) each entry is exactly 8 bytes = two 32-bit ints, (b) entries are deduplicated
by full 8-byte equality, (c) the count is tracked in int-units (incremented by 2 per entry), and
(d) the source data is an internal array with 8-byte stride at `*(base + 0x18)`.

### 1.7 Count helper `FUN_101a8bd0` (callee)

Called as **fastcall**: `ECX = param_1` (context), `EDX = param_2`, stack = `param_3` (key).
```
101a8f73  MOV EDX,dword ptr [EBP + 0xc]   ; EDX = param_2
101a8f78  MOV ESI,dword ptr [EBP + 0x8]   ; ESI = param_1
101a8f7b  MOV ECX,ESI                     ; ECX = param_1  (fastcall this/arg1)
101a8f7e  PUSH dword ptr [EBP + 0x10]     ; stack arg = param_3
101a8f81  CALL 0x101a8bd0
```
Returns an `int` = number of dependency entries for the given context/key. Used only to size the
allocation. When `param_1 == 0` it sums counts across the global tree `DAT_1023dccc`; when
`param_1 != 0` it computes `(end - begin) >> 3` (i.e. byte span / 8) of a single object's
internal 8-byte-stride array — consistent with the 8-byte entry size.

---

## 2. `_DeleteDependencyList@4` (ordinal 397, `0x101A9150`)

### 2.1 Calling convention — VERIFIED stdcall, 1 param (4 bytes)

**Evidence (full disassembly — the function is tiny):**
```
101a9150  PUSH EBP
101a9151  MOV EBP,ESP
101a9153  PUSH 0x4
101a9155  PUSH dword ptr [EBP + 0x8]    ; param_1 (the buffer pointer)
101a9158  CALL 0x101eae93              ; -> operator delete wrapper
101a915d  ADD ESP,0x8                  ; cleanup 2 pushed args (cdecl callee)
101a9160  POP EBP
101a9161  RET 0x4                      ; callee cleans 4 bytes -> stdcall, 1 x 4-byte param
```
`RET 0x4` (ret 4) is the definitive stdcall signature: the callee pops 4 bytes (= 1 × 4-byte
parameter). The `@4` decoration is correct.

Ghidra decompiled signature:
```c
void _DeleteChangedModulesList_4(undefined4 param_1)
```
(Return type `void`, single `undefined4` parameter.)

### 2.2 Parameter types/meaning

| Param | Stack | C type (inferred) | Meaning |
|---|---|---|---|
| `param_1` | `EBP+8` | `void *` (pointer) | **The buffer pointer returned by `_GetStaticDependencyList@12`.** It is forwarded directly to the delete wrapper `FUN_101eae93`, which calls `operator delete(param_1)`. |

### 2.3 Return type

**`void`.** No value returned (`POP EBP; RET 0x4` with EAX untouched after the call).

### 2.4 Interaction with the dependency list memory — VERIFIED

`_DeleteDependencyList@4` does **one thing**: it frees the buffer via C++ `operator delete`.

**Evidence — the wrapper `FUN_101eae93` decompilation:**
```c
void FUN_101eae93(void *param_1)
{
  operator_delete(param_1);
  return;
}
```
The call site pushes `0x4` then `param_1` before calling `FUN_101eae93`. The `0x4` is the second
argument to the cdecl wrapper (a size/flag hint, matching the MSVC scalar-deleting-destructor
pattern where a `size_t` hint is passed to `operator delete`). `FUN_101eae93`'s decompilation
shows it only uses `param_1` and ignores the size hint, calling `operator_delete(param_1)`.

**Conclusion:** `_DeleteDependencyList@4(ptr)` is the ownership-release counterpart to
`_GetStaticDependencyList@12(...)`. It frees exactly the buffer that
`_GetStaticDependencyList@12` allocated with `operator new`. The caller must pass the same
pointer that was returned by `_GetStaticDependencyList@12`. Passing `NULL` is **not** guarded
against in this thin wrapper (it would call `operator delete(NULL)`, which is a well-defined
no-op for standard C++ `operator delete`, so it is safe in practice).

### 2.5 Does it free memory from `GetDependencies`?

There is no function named `GetDependencies` in the analyzed region. `_DeleteDependencyList@4`
frees the buffer returned by **`_GetStaticDependencyList@12`** (and, being an alias of
`_DeleteChangedModulesList@4`, presumably also buffers returned by a corresponding
`_GetChangedModulesList`-style producer that uses the same `operator new` allocation pattern).
The matching is by **allocation contract** (same `operator new`/`operator delete` pair), not by
any type tag stored in the buffer.

---

## 3. Cross-references (caller analysis)

### 3.1 Xrefs to `_GetStaticDependencyList@12` (`0x101A8F70`)
```
from_address   type
-----------    --------
0x1021D7DC     DATA          (export table slot — ordinal 531)
Entry Point    EXTERNAL      (the export itself)
```
**No direct CALL xrefs from within orlib20.dll.** The function is only referenced by the export
table — i.e. it is intended to be called by external clients (e.g. TSDev.exe) via
`GetProcAddress` by ordinal 531 or by name.

### 3.2 Xrefs to `_DeleteDependencyList@4` (`0x101A9150`)
```
from_address   type
-----------    --------
0x1021D5C0     DATA          (export table slot — ordinal 396, _DeleteChangedModulesList@4)
0x1021D5C4     DATA          (export table slot — ordinal 397, _DeleteDependencyList@4)
Entry Point    EXTERNAL      (the export itself)
```
**No direct CALL xrefs from within orlib20.dll.** Two export-table DATA references confirm the
aliasing (two ordinals pointing at the same address).

### 3.3 External caller search

The other programs in the Ghidra project (`TSDev.exe`, `ELRte.dll`, `ELMDCore.dll`, `ECL.dll`,
`ORDllHst.exe`, `ORPlat.exe`, `orchart.exe`, `ELIS.exe`, `tselc.dll`, `tslib.dll`,
`ELDebugger.dll`, `TSResourceDllEng.dll`) were checked for imports of
`_GetStaticDependencyList@12` / `_DeleteDependencyList@4` / `_DeleteChangedModulesList@4` by
name. **No named import of these functions was found in any loaded program.**

This is consistent with the functions being **ordinal-only exports** consumed via
`GetProcAddress(hOrlib20, MAKEINTRESOURCE(531))` / `MAKEINTRESOURCE(397)` at runtime (a common
pattern for internal TS APIs). The actual call sites are therefore not statically resolvable in
the current Ghidra project; they would only appear as indirect calls through a resolved function
pointer. **Dynamic tracing (Frida) would be required to confirm the real callers and the argument
values they pass** — that is out of scope for this static ABI note.

**Implication for ownership:** Because no internal caller exists, the only evidence of the
get/delete pairing is the allocation/free contract (§1.4 + §2.4): `operator new` in
`_GetStaticDependencyList@12` is matched by `operator delete` in `_DeleteDependencyList@4`. The
caller (external) is responsible for the get-then-delete lifecycle.

---

## 4. Confirmed C signatures

```c
/* ordinal 531, stdcall, 12 bytes params, callee-cleaned */
/* Returns a heap-allocated int buffer (caller must free via _DeleteDependencyList@4). */
int * __stdcall _GetStaticDependencyList@12(
    int  param_1,   /* context selector: 0 = global tree walk; nonzero = single object ptr (this) */
    int  param_2,   /* header[0] value stored at offset 0 of the returned buffer */
    int  param_3    /* header[1] value stored at offset 4; also the lookup key for the dep map */
);

/* ordinal 397 (alias of ordinal 396 _DeleteChangedModulesList@4), stdcall, 4 bytes params */
/* Frees the buffer returned by _GetStaticDependencyList@12 via operator delete. */
void __stdcall _DeleteDependencyList@4(
    int * param_1   /* buffer pointer returned by _GetStaticDependencyList@12; NULL is safe */
);
```

Equivalent Win32-decorated prototypes (for a header):
```c
__declspec(dllimport) int *  __stdcall _GetStaticDependencyList@12(int, int, int);
__declspec(dllimport) void   __stdcall _DeleteDependencyList@4(int *);
```

---

## 5. Returned dependency list format (summary)

```
struct StaticDependencyList {       // packed, 4-byte aligned
    int  header0;                  // offset  0: = param_2
    int  header1;                  // offset  4: = param_3
    struct {                       // offset  8: N entries, 8 bytes each
        int a;                     //   +0  (dependency field 0 — meaning UNDETERMINED)
        int b;                     //   +4  (dependency field 1 — meaning UNDETERMINED)
    } entries[N];                  //   deduplicated by full {a,b} equality
    int  term0;                    // offset  8+8N: 0xFFFFFFFF
    int  term1;                    // offset 12+8N: 0xFFFFFFFF
};
```
- `N` is not stored; it is implied by the terminator position: `N = (term_int_index - 2) / 2`.
- Entries are deduplicated (full 8-byte equality) at production time.
- The buffer is a single `operator new` allocation; free it with one call to
  `_DeleteDependencyList@4`.

---

## 6. Caveats / UNDETERMINED items

1. **Semantic meaning of `param_2` and `param_3`** (the two header ints): statically
   UNDETERMINED. They are stored verbatim into the buffer header and used as a lookup key
   (`param_3`) / fastcall arg (`param_2`) into the internal dependency map. Likely a
   {context/type, key} pair, but this cannot be proven from the decompilation. Dynamic tracing
   (Frida) of a real caller is needed to confirm.
2. **Semantic meaning of the two ints in each entry** (`a`, `b`): statically UNDETERMINED. They
   are read from an internal 8-byte-stride array at `*(obj + 0x18)`. Likely a
   {dependency-kind, dependency-id} pair, but not provable from static analysis.
3. **Real callers / argument values**: no static CALL xrefs exist inside orlib20.dll, and no
   named imports exist in any other loaded program. The functions are ordinal-only exports
   consumed via `GetProcAddress` by ordinal. Call-site argument values must be captured
   dynamically (Frida) — out of scope for this note.
4. **`_DeleteChangedModulesList@4` (ordinal 396) vs `_DeleteDependencyList@4` (ordinal 397)**:
   confirmed to be the **same address / same body** (aliasing). Whether there is a separate
   `_GetChangedModulesList` producer that also feeds this deleter is not analyzed here (out of
   scope); the deleter is allocation-contract-compatible with any `operator new` buffer, but the
   only producer verified in this note is `_GetStaticDependencyList@12`.
5. **NULL-safety of `_DeleteDependencyList@4`**: not explicitly guarded in the wrapper, but
   standard C++ `operator delete(NULL)` is a defined no-op, so passing NULL is safe in practice.
6. **Calling convention of the helpers** (`FUN_101a8bd0` = fastcall, `FUN_101a8e60` = thiscall)
   was determined from the call-site register/stack usage, not from Ghidra's (sometimes
   imprecise) decompiled signatures. The decompiler labeled `FUN_101a8e60` as `__fastcall` with
   5 params, but the call site uses `ECX` as `this` and only 3 stack args (`ADD ESP,0xC` cleanup),
   confirming thiscall semantics.
