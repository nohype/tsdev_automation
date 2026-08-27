# RE: `GetDependencies` (orlib20.dll, ordinal 487, RVA 0x153F00)

**Source:** Ghidra project `TSDevAnalysis`, program `orlib20.dll`.
**Function address (image-relative):** `0x10153F00` (image base `0x10000000` + RVA `0x153F00`).
**Decompilation tools used:** `decompile_function`, `disassemble_function`, `get_function_variables`, `get_function_xrefs`, `decompile_function` on callees (`0x10153ad0`, `0x101a39e0`, `0x101cff60`, `0x101d43a0`, `0x101d4280`, `0x1015df30`, `0x1014f630`, `0x1001c890`).
**Task scope:** Read-only RE verification of the ABI. No source code modified.

> **Use note:** This is an ABI/reference record for the forward-dependency export,
> not the current M.4 reverse-dependent implementation. The M.4 evidence found
> that this export checks `0xFA4` while compiler-written function references use
> `0xFA3`; production therefore uses the linker-based path documented in
> `docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md` §8.7.

---

## 1. Confirmed ABI (C signature)

```c
/* ordinal 487, __stdcall, 4 args, callee cleans 16 bytes (RET 0x10) */
HGLOBAL __stdcall GetDependencies(int   whType,
                                  int   progId,
                                  int   elementIndex,
                                  int * outDepCount);
```

Equivalently, since `HGLOBAL` is `void *` on Win32:

```c
void * __stdcall GetDependencies(int whType, int progId, int elementIndex, int * outDepCount);
```

The previous implementation's signature
`HGLOBAL (__stdcall *)(int whType, int progId, int elementIndex, int* outDepCount)`
is **ABI-correct** (calling convention, parameter count, parameter widths, and return width all match). The crash was therefore **not** caused by a wrong ABI on this function — see "Why the previous implementation may have crashed" below for the actual hazards.

---

## 2. Evidence for each conclusion

### 2.1 Calling convention = `__stdcall`, 4 parameters, callee-cleaned

Disassembly of `GetDependencies` at `0x10153F00` ends with:

```
10153fb3  RET 0x10
```

`RET imm16` with `imm16 = 0x10` (16 bytes) is the x86 stdcall epilogue where the callee pops 4 dword arguments (4 × 4 = 16). A cdecl function would use a bare `RET` and leave the caller to clean up via `ADD ESP,imm` after the `CALL`. No such `ADD ESP` follows the call site inside `GetDependencies`' own body for its own arguments (the `ADD ESP,0x8` / `ADD ESP,0xc` inside the body are for the cdecl-style helpers it calls, not for `GetDependencies` itself).

Prologue confirms a standard frame:

```
10153f00  PUSH EBP
10153f01  MOV EBP,ESP
10153f03  SUB ESP,0x148
```

The four incoming arguments are accessed at the canonical stdcall stack offsets:

```
10153f29  MOV  ESI,dword ptr [EBP + 0x8]    ; param_1 (whType)
10153f2c  CMP  ESI,0x32                     ; if (whType == 0x32) skip
10153f44  PUSH dword ptr [EBP + 0xc]        ; param_2 (progId)  -> passed to FUN_101cff60
10153f74  MOV  EDX,dword ptr [EBP + 0xc]    ; param_2 (progId)  -> passed to FUN_101a39e0
10153f81  MOV  EDX,dword ptr [EBP + 0x10]   ; param_3 (elementIndex) -> thiscall ECX/EDX into FUN_10153ad0
10153f86  MOV  EAX,dword ptr [EBP + 0x14]   ; param_4 (outDepCount pointer)
10153f8c  MOV  dword ptr [EAX],EBX          ; *outDepCount = 0  (EBX was zeroed at 10153f12)
```

Ghidra's decompiler agrees and emits the prototype:

```c
undefined4 GetDependencies(int param_1, undefined4 param_2, undefined4 param_3, undefined4 *param_4)
```

(`undefined4` return is just Ghidra's default before a type is applied; the body returns `EBX`, which holds the `HGLOBAL` from `FUN_10153ad0` — see §2.4.)

### 2.2 Parameter 1 — `int whType` (warehouse/component type)

- Loaded into `ESI` at `10153f29` (`MOV ESI,[EBP+8]`).
- Compared against `0x32` (`CMP ESI,0x32` / `JZ 0x10153fa0`): if `whType == 0x32` (decimal 50) the function skips the dependency-collection body and returns 0. `0x32` is the same sentinel used elsewhere in orlib20 to mean "no warehouse type / not applicable" (see `FUN_101a39e0` which masks `param_1 & 0xff` and tests `(param_1 & 0xff) != 0x32`).
- Otherwise `whType` is passed as the first argument to `FUN_101d43a0(whType, &local_40)`, which is a **type → power-type-array lookup table**:

  ```c
  void FUN_101d43a0(undefined4 param_1, undefined4 *param_2) {
    switch(param_1) {
      case 0: *param_2=0; param_2[1]=0x200;   param_2[2]=0xffffffff; return;
      case 1: *param_2=1; param_2[1]=0xffffffff; return;
      case 2: *param_2=2; param_2[1]=0x502; param_2[2]=0x402; ... param_2[0xb]=0xffffffff; return;
      case 3: *param_2=3; param_2[1]=0xffffffff; return;
      case 4: *param_2=4; param_2[1]=0xffffffff; return;
      default: *param_2=0xffffffff; return;
    }
  }
  ```

  This is the same "whType" taxonomy used across orlib20 (0 = Strategy, 1 = ActivityBar, 2 = Indicator, 3 = PaintBar, 4 = Function, etc., matching `docs/TS_COMPONENT_TYPES.md`). The values written (e.g. `0x200`, `0x502`, `0x402`, …) are the "power-type" codes consumed by `FUN_101d4280` / `ConvertToPowerType`.

**Conclusion:** param_1 is the warehouse component type (`whType`). Same meaning as the `whType` argument used by `_LoadMasterRecsEX@16`, `_InsertItemMaster@*`, etc.

### 2.3 Parameter 2 — `int progId` (program/record id)

- Read at `[EBP+0xc]` (`10153f44`, `10153f74`).
- Passed as the **second** argument to `FUN_101cff60(powerTypeArray, progId, &local_14c)` and as the second argument to `FUN_101a39e0(whType, progId, …)`.
- `FUN_101cff60` uses `(powerTypeArray, progId)` to look up a warehouse record pointer (`FUN_101a9460` / `FUN_101e3c50` / `FUN_101d17b0` all take `progId` as the record-id key). This is the same `progId` (a.k.a. "record id" / "program id") used by every other orlib20 warehouse API in this project (e.g. `_LoadMasterRecsEX@16`'s progId, `_DeleteDependencyList@4`'s argument).

**Conclusion:** param_2 is the warehouse program/record id (`progId`). It is an `int`, not a pointer.

### 2.4 Parameter 3 — `int elementIndex` (function/element index inside the record)

- Read at `[EBP+0x10]` (`10153f81`: `MOV EDX,[EBP+0x10]`).
- It is forwarded as the **second** parameter to `FUN_10153ad0` (the actual dependency-list builder). The disassembly of `FUN_10153ad0` shows it is received in `EDX` (thiscall-style: `ECX` = `this`/context, `EDX` = first explicit arg, `[EBP+8]` = second explicit arg):

  ```
  10153ad9  MOV  ESI,EDX                  ; ESI = elementIndex (param_3 of GetDependencies)
  10153ae8  CMP  ESI,dword ptr [ECX+0x29] ; if (elementIndex >= record->count) -> bail
  10153af8  IMUL EBX,ESI,0xb5             ; elementIndex * 0xb5 (struct stride) + record->elements
  10153afe  ADD  EBX,EDI                  ; EBX = &record->elements[elementIndex]
  ```

  I.e. `elementIndex` selects one element (one function/strategy slot) out of the record's element array (stride `0xb5` = 181 bytes per element). The dependency list is built **for that single element**.

- `GetDependencies` also passes `elementIndex` (via `EDX`) into `FUN_101a39e0`, which itself forwards it as the "element index" to `FUN_101a3340`. Same meaning.

**Conclusion:** param_3 is a 0-based index into the record's element array (`elementIndex`). It is an `int`, not a pointer. Valid range is `[0, record->elementCount)`; out-of-range yields a NULL return and `*outDepCount = 0`.

### 2.5 Parameter 4 — `int * outDepCount` (OUT: number of dependencies written)

- Read at `[EBP+0x14]` (`10153f86`: `MOV EAX,[EBP+0x14]`).
- Immediately written to **before** the list is built:

  ```
  10153f8c  MOV dword ptr [EAX],EBX   ; *outDepCount = 0   (EBX == 0 here)
  ```

  This zero-initializes the count so that if the builder returns NULL the caller sees `*outDepCount == 0`.

- The same pointer is then passed as the third argument to `FUN_10153ad0(context, elementIndex, &outDepCount_local)` where it is used as an **in/out count**:

  ```c
  /* FUN_10153ad0 (decompiled) */
  HGLOBAL FUN_10153ad0(int ctx, int elementIndex, int *param_3) {
    HGLOBAL local_8 = NULL;
    if (ctx != 0) {
      int base = *(int *)(ctx + 0x4f);
      if (elementIndex < *(int *)(ctx + 0x29)) {
        local_8 = GlobalAlloc(0x40, 1);                 // GMEM_MOVEABLE | GMEM_ZEROINIT
        int off = elementIndex * 0xb5 + base;
        int *p   = *(int **)(off + 0x7b);               // element's dep-record list
        int *cur = p;
        if (*(int *)(off + 0x73) != 0) {                // dep-record list length
          do {
            if (*cur == 0xfa4) {                        // opcode 0xFA4 = "function ref"
              int n = CELBasicCompiler::ExpandFuncList(*(int *)((int)cur + 0xd),
                                                       &local_8, *param_3);
              *param_3 = n;                             // count grows by 1 per unique ref
            }
            DAT_1023ceac = *(undefined4 *)((int)cur + 9);
            cur = (int *)((int)cur + (uint)*(byte *)(cur + 1));   // advance by record length
          } while ((uint)((int)cur - (int)p) < *(uint *)(off + 0x73));
        }
        if (*param_3 < 1) { GlobalFree(local_8); local_8 = (HGLOBAL)0x0; }
      }
      return local_8;
    }
    return local_8;
  }
  ```

  So `*outDepCount` is incremented once for every **unique** function reference added to the list (uniqueness enforced by `ExpandFuncList`, which scans the existing list and skips duplicates — see §4).

**Conclusion:** param_4 is an OUT parameter: a pointer to a single `int` that receives the number of dependency entries written into the returned HGLOBAL buffer. The caller must pass a valid pointer to an `int` (the function writes `0` to it unconditionally before building, then updates it as entries are added).

### 2.6 Return type = `HGLOBAL` (a Win32 global memory handle), or NULL

- The function returns `EBX` (`10153fad: MOV EAX,EBX`).
- `EBX` is loaded from the return value of `FUN_10153ad0` at `10153f96: MOV EBX,EAX`.
- `FUN_10153ad0` is typed by Ghidra as `HGLOBAL __fastcall FUN_10153ad0(...)` and its body allocates the return value with `GlobalAlloc(0x40, 1)` and frees failures with `GlobalFree(local_8)`. `0x40` = `GMEM_MOVEABLE | GMEM_ZEROINIT`.
- The growth path inside `CELBasicCompiler::ExpandFuncList` reallocates via `Lib_GlobalReAlloc`, which is itself a `GlobalAlloc`/`GlobalFree`-based wrapper (decompiled at `0x1014f630`):

  ```c
  HGLOBAL Lib_GlobalReAlloc(HGLOBAL h, uint size, uint flags) {
    SIZE_T cur = GlobalSize(h);
    if (h == NULL) { SetLastError(0); return GlobalAlloc(flags & 0xfffffffd, size); }
    if (size == cur) return h;
    SetLastError(0);
    HGLOBAL dst = GlobalAlloc(flags & 0xfffffffd, size);
    if (dst) memcpy(dst, h, min(cur,size));
    GlobalFree(h);
    return dst;
  }
  ```

**Conclusion:** the return value is a **moveable Win32 `HGLOBAL`** (allocated with `GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT, …)`), or `NULL` on failure / no dependencies. It is **not** a raw pointer and **not** a `malloc`-style heap pointer. To read it the caller must `GlobalLock` it; to free it the caller must `GlobalFree` it (see §3).

The decompiler's `undefined4` return type is just the default placeholder; the body's use of `GlobalAlloc`/`GlobalFree` and the `HGLOBAL` typing of `FUN_10153ad0` establish the real type.

---

## 3. Memory ownership and how to free the returned handle

**Ownership:** The handle is allocated **inside** `GetDependencies` (via `FUN_10153ad0` → `GlobalAlloc`/`Lib_GlobalReAlloc`) and **transferred to the caller**. `GetDependencies` itself does **not** retain a reference and does **not** free the handle on success. On the failure/no-deps path the function frees the handle internally and returns NULL (so the caller must not free a NULL return).

**Free function:** `GlobalFree` (Win32 kernel32). Evidence:

- `FUN_10153ad0` uses `GlobalFree(local_8)` to discard its own allocation on the no-deps path.
- `Lib_GlobalReAlloc` uses `GlobalFree(param_1)` to discard the old block after copying.
- The allocation flags are `0x40` (`GMEM_MOVEABLE | GMEM_ZEROINIT`), so the matching deallocator is `GlobalFree` (not `free`, not `LocalFree`, not `HeapFree`).

**Do NOT use `_DeleteDependencyList@4`:** that is a different orlib20 export that operates on a warehouse-side dependency-list structure (the `record->elements[i]` dep list at offset `+0x7b`), not on the `HGLOBAL` returned by `GetDependencies`. Mixing them up is a likely cause of crashes (see §6).

**Reading the data:** Because the handle is `GMEM_MOVEABLE`, the caller should `GlobalLock(h)` to obtain a usable pointer, read the `*outDepCount` `int`s, then `GlobalUnlock(h)` before/after `GlobalFree(h)`. (In practice, on modern Win32 a moveable block's base address is stable, but the contract still requires `GlobalLock`/`GlobalUnlock`.)

---

## 4. Format of the returned dependency list

The returned `HGLOBAL` is a flat **array of 32-bit ints**, one per dependency. There is **no header, no struct, no count prefix** — the count is delivered separately through `*outDepCount`.

Evidence — `CELBasicCompiler::ExpandFuncList` (decompiled at `0x1001c890`, mangled name `?ExpandFuncList@CELBasicCompiler@@SAHHPAPAXH@Z` → `public: static int __cdecl CELBasicCompiler::ExpandFuncList(int, void**, int)`):

```c
int __cdecl CELBasicCompiler::ExpandFuncList(int funcId, void **param_2, int curCount) {
  int i = 0;
  if (0 < curCount) {
    do {
      if (*(int *)((int)*param_2 + i * 4) == funcId)   // already in list?
        return curCount;                               // dedup: don't add twice
      i = i + 1;
    } while (i < curCount);
  }
  int newBytes = (curCount + 1) * 4;                    // grow by exactly one int
  void *p = Lib_GlobalReAlloc(*param_2, newBytes, 0x40);
  *param_2 = p;
  if (p != NULL) {
    *(int *)((newBytes - 4) + (int)p) = funcId;        // append funcId at the end
    return curCount + 1;
  }
  FUN_10154780(0x7532, -1, -1, -1);                    // error 0x7532 = "out of memory"
  return 0;
}
```

Key facts derived from this:

1. **Element size = 4 bytes** (`i * 4`, `(curCount+1) * 4`, `newBytes - 4`).
2. **Element type = `int`** (each entry is a function id written via `*(int *)…`).
3. **Layout = packed array of ints, no header.** The buffer is grown one `int` at a time via `Lib_GlobalReAlloc`; the new entry is written at offset `(curCount)*4`. There is no length prefix and no trailing sentinel.
4. **Entries are deduplicated.** `ExpandFuncList` scans the existing `[0, curCount)` entries and returns early if `funcId` is already present, so the returned list contains **unique** function ids.
5. **Entries are function ids (the `progId`/record-id of a Function-type warehouse record).** The value appended is `funcId`, which comes from `*(int *)((int)cur + 0xd)` in `FUN_10153ad0` — i.e. the operand of the `0xFA4` "function reference" opcode in the element's compiled bytecode stream. These are the same numeric ids used to look up Function records elsewhere in orlib20.
6. **Count is delivered out-of-band** via `*outDepCount` (param_4). The buffer's size in bytes is therefore `(*outDepCount) * 4`, and `GlobalSize(h)` will report exactly that (since `Lib_GlobalReAlloc` allocates `(curCount+1)*4` bytes on each grow).

So a correct read loop is:

```c
int count = 0;
HGLOBAL h = GetDependencies(whType, progId, elementIndex, &count);
if (h) {
    int *deps = (int *)GlobalLock(h);   // count ints, no header
    for (int i = 0; i < count; i++) use(deps[i]);
    GlobalUnlock(h);
    GlobalFree(h);
}
```

---

## 5. Side effects

1. **Writes `*outDepCount`** unconditionally (to `0` on entry, then incremented as deps are added). Caller must pass a writable `int *`.
2. **Writes the global `DAT_1023ceac`** (an `undefined4` at `0x1023ceac`) on every iteration of the dependency-record scan in `FUN_10153ad0`:
   ```c
   DAT_1023ceac = *(undefined4 *)((int)cur + 9);
   ```
   This is a "last-seen dep-record field" scratch global shared across the process. It is not thread-safe and not re-entrant; concurrent calls to `GetDependencies` (or anything else that touches `DAT_1023ceac`) will race. Single-threaded use from the automation CLI is fine.
3. **Calls `FUN_101bff20()` at entry** (a TLS/thread-affinity check also used by other orlib20 entry points) and asserts the result of `FUN_101a9650(0)` is non-NULL via `INT3` (`swi(3)`) if the runtime context isn't initialized. Practically: `GetDependencies` must be called **after** the orlib20 runtime/warehouse is initialized (same requirement as every other orlib20 API in this project). Calling it before init traps the process with `INT3`.
4. **Calls `guard_check_icall()`** before returning (CFG-style indirect-call guard). No caller action required.
5. **Allocates and frees Win32 global memory** (`GlobalAlloc` / `GlobalFree` / `Lib_GlobalReAlloc`) — see §3. On the success path the final allocation is handed to the caller; on the no-deps/failure path it is freed internally.
6. **Does not modify the warehouse record.** The element's dep-record list at `record->elements[i] + 0x7b` is only **read** (`*cur`, `cur + 0xd`, `cur + 1` for stride). `GetDependencies` is a pure query w.r.t. the on-disk warehouse state.

---

## 6. Why the previous implementation may have crashed (ABI was correct)

The ABI itself is correct; the crash is more likely explained by one of the following hazards, all of which are visible in the decompilation:

1. **Treating the return as a raw pointer / wrong free function.** The handle is `GMEM_MOVEABLE`. Calling `free()` on it, or calling `_DeleteDependencyList@4` on it (which expects a *warehouse-side* dep-list node, not a `GlobalAlloc`'d int array), will corrupt the heap / crash. Must use `GlobalFree`.
2. **Not locking before reading.** Strictly, `GlobalLock`/`GlobalUnlock` should bracket access to a `GMEM_MOVEABLE` block. Skipping the lock and dereferencing the handle directly is undefined per the Win32 contract (works on modern NT, but is the kind of latent bug that bites on certain code paths).
3. **Passing a NULL `outDepCount`.** `GetDependencies` writes `*param_4 = 0` at `10153f8c` **before** checking anything. A NULL param_4 traps immediately with an access violation — independent of `whType`/`progId`/`elementIndex`.
4. **Out-of-range `elementIndex`.** `FUN_10153ad0` guards with `if (elementIndex < *(int *)(ctx + 0x29))`; an out-of-range index returns NULL with `*outDepCount = 0` (no crash), but if the caller then treats NULL as a valid pointer it will crash on deref.
5. **Calling before runtime init.** The `FUN_101a9650(0)` check + `INT3` will hard-trap the process if the warehouse/TLS context isn't up. The CLI must have already initialized orlib20 (STA COM apartment, `_LoadMasterRecsEX@16` warm-up, etc.) before calling `GetDependencies`.
6. **`whType == 0x32` (50).** This short-circuits to a NULL return with `*outDepCount` left at whatever the caller set (the `*param_4 = 0` write happens **only on the non-0x32 path**, after `FUN_101a39e0` returns — see `10153f8c` is inside the `if (iVar2 != 0 && (local_d3 & 4))` block). If the caller passes `whType = 0x32` and then reads `*outDepCount` expecting it to be zeroed, it will read uninitialized memory. The caller must initialize `outDepCount = 0` before the call.

---

## 7. UNDETERMINED items

- **External callers in `TSDev.exe`.** `get_function_xrefs` for `0x10153F00` returns only two references: a DATA reference from `0x1021d72c` (the orlib20 **export table** entry for ordinal 487) and the `Entry Point` external marker. No direct CALL site exists inside orlib20 itself, and `TSDev.exe` does not statically import `GetDependencies` by name (`list_imports` filtered for "GetDependencies" returns no match; the import table uses ordinals/names that do not include this symbol). TSDev most likely resolves it **by ordinal** via `GetProcAddress(hOrlib20, (LPCSTR)487)` at runtime, so the static call site is not visible in Ghidra. **The exact argument values TSDev passes cannot be confirmed from static analysis alone** — but the function body's own usage (§2.2–2.5) fully constrains the meaning of each parameter, so the ABI is determinable without the caller.
- **Precise numeric range of `progId`.** It is the warehouse record id (same space as the `progId` accepted by `_LoadMasterRecsEX@16` etc.), but the legal range / sentinel values are governed by the warehouse, not by `GetDependencies`. UNDETERMINED from this function alone.
- **Whether the `int` entries in the returned array are *Function* progIds specifically, or a broader "any referenced record id".** The only append path visible (`opcode 0xFA4` → `ExpandFuncList`) appends function-reference ids. Other opcodes that might also feed the list were not observed in this function's body, but the surrounding `do/while` only acts on `0xFA4`, so within `GetDependencies` the list is function-reference ids. If other orlib20 code paths populate the same `HGLOBAL` layout with non-function ids, that is outside this function's scope.
- **Thread-safety of `DAT_1023ceac`.** Confirmed not re-entrant (single shared global, no lock around its write in `FUN_10153ad0`); whether any other orlib20 API also writes it is not exhaustively verified here.
