# RE_M4 — Reverse-Dependent Revalidation (`FUN_004D91E0`)

> **Status:** Research-only RE evidence for M.4b.1/M.4b.2. Reference document.
> **Date:** 2026-08-26 (§9.8 updated 2026-08-28)
>
> **Scope:** RE of `TSDev.exe!FUN_004D91E0`, the reverse-dependent reverify
> wrapper between `FUN_004D9480` and `orlib20!_VerifyModule_New@20`.
> **Method:** Ghidra static analysis of `TSDev.exe`, `orlib20.dll`,
> `WHServer.exe` in `TSDevAnalysis`. PE export-table parsing via `pefile`.
> **Binaries:** `C:\Program Files (x86)\TradeStation 9.5\Program\TSDev.exe`,
> `...\orlib20.dll` (baseline `9.50.01.3344`).
>
> **Correction history (gate logic):** `FUN_004d9040` returns 1 = no change,
> 0 = changed. `local_24 = (uint)(cVar3 == '\0')` = 1 when changed. Gate
> `if ((iVar7 == 0) || (local_24 == 1))` = recurse if (NOT verified OR changed);
> skip if (verified AND NOT changed). The contract-change check compares the
> ROOT's old vs new record (done once), not each dependent's record.
>
> **Supersession history:** §§5.3–5.4 (Option B2 reverse-by-inverting-forward)
> and §6 are superseded. §8.7 linker-based approach is superseded by §9 (M.4a):
> the warehouse provides reverse dependents via cmd `0x3B` (`IProgram::Callers`),
> not by enumerating master records or parsing linker dep-lists. M.4b replaces
> the linker-based approach with this warehouse-level mechanism. See
> `docs/workstream_tsdev_parity_plan.md` §M.4.

---

## 0. Executive Summary

`FUN_004D91E0` is **TSDev.exe-private** (not in orlib20.dll). Behavior:

1. **Verifies the current module** via `orlib20!_VerifyModule_New@20` (ordinal 700) — full `CompileText` recompile.
2. **Only on success**, for categories 0/1/4 only, **enumerates reverse dependents** from a TSDev-internal `CELPrograms` BST populated from the orlib20 warehouse (worker-thread cmd `0x0D` = `IWHouse::ProgramById` for the root record, then cmd `0x3B` = `IProgram::Callers` for the reverse-dependent set; or cached BST via `Ordinal_71`/`InitGlobalLookup`).
3. **Metadata-based contract-change comparison** (`FUN_004d9040`) between old and new root records — purely warehouse-record metadata, NO source-text parsing (R-M.4.2, AGENTS.md-compliant).
4. **Recurses** into itself (`0x004D9402`) per reverse dependent, gated by per-record "verified" flag (`*(byte*)(record+0x55) & 1`) and contract-change result. **No visited-set, no depth counter.** (R-M.4.3.)
5. Per-dependent failures **accumulated, not aborting**: `param_1[1] = param_1[1] & uVar6`. Root result (`+0x00`) set only when primary-flag=1. (R-M.4.4.)

**orlib20 does NOT export a reverse-dependent API.** The traversal is TSDev.exe-private code driving the warehouse via:
`Ordinal_10` (GetWarehouse, RVA `0x1A9650`), `Ordinal_71` (InitGlobalLookup, RVA `0x1A9460`), `Ordinal_28` (RVA `0x1A03F0`), `Ordinal_69` (RVA `0x1A3E00`), `Ordinal_68` (RVA `0x1A87D0`), `Ordinal_70` (RVA `0x15DF30`), `_VerifyModule_New@20` (ordinal 700, RVA `0x169630`).

orlib20 exports **forward** dependency APIs only (`GetDependencies` ord 487, `_GetDynamicDependencyList@12` ord 489, `_GetStaticDependencyList@12` ord 531, `_DeleteDependencyList@4` ord 397). `GetDependencies` is broken (§8.1: checks `0xFA4`, compiler writes `0xFA3`).

**M.4a (§9) resolved the BST population gap:** reverse dependents come from cmd `0x3B` (`IProgram::Callers`), not master-record enumeration or linker dep-lists. M.4b replaces the linker-based approach with this warehouse-level mechanism.

---

## 1. R-M.4.1 — Exact Traversal

### 1.1 Function signature and entry

`FUN_004D91E0` at `TSDev.exe:0x004D91E0` is `__thiscall` (`ECX` = compiler object).

```asm
004d9201  MOV EDI,[EBP + 0x8]    ; param_2 (stack) = category (whType)
004d9204  MOV EBX,ECX           ; this = compiler object
```

Caller `FUN_004D9480` (`0x004D9480`) setup:

```asm
004D9488  PUSH dword ptr [ESI+0x10]   ; arg3 = compiler+0x10 = ProgId
004D948B  MOV  ECX,ESI               ; this = compiler object
004D948D  MOV  dword ptr [ESI],0      ; compiler+0x00 = 0 (root result)
004D9493  PUSH dword ptr [ESI+0x0C]   ; arg2 = compiler+0x0C = subtype
004D9496  MOV  dword ptr [ESI+4],0    ; compiler+0x04 = 0 (aggregate result)
004D949D  PUSH dword ptr [ESI+8]      ; arg1 = compiler+0x08 = category
004D94A0  CALL 0x004D91E0
```

```c
void __fastcall FUN_004d9480(undefined4 *param_1) {
    StartCompilingProcess();
    *param_1 = 0;            // compiler+0x00 = 0
    param_1[1] = 0;         // compiler+0x04 = 0
    FUN_004d91e0(param_1[2], param_1[3], param_1[4]);  // category, subtype, ProgId
    EndCompilingProcess();
}
```

`FUN_004D91E0(this=compiler, category, subtype, ProgId)`. Compiler object layout
(confirmed by M.1, `docs/M1_HANDOFF.md` §R-M.1.2): `+0x08`=category, `+0x0C`=subtype, `+0x10`=ProgId, `+0x18`=HWND. `+0x00` and `+0x04` are result accumulators zeroed before the call.

### 1.2 Step 1 — Verify the current (root) module

After category normalization via `FUN_004d7a30`, calls `_VerifyModule_New@20` (`0x004D9299`):

```asm
004d928b  PUSH EBX                      ; arg5 = compiler object
004d928c  PUSH ECX                      ; arg4 = primary-flag (1 if root==this)
004d928d  PUSH EAX                      ; arg3 = ProgId
004d928e  PUSH EDI                      ; arg2 = category (whType)
004d928f  PUSH dword ptr [EBX + 0x18]   ; arg1 = compiler+0x18 (HWND)
004d9299  CALL 0x0053f5bc               ; _VerifyModule_New@20 thunk
```

Primary-flag (`ECX`) computed at `0x004d9278`–`0x004d928b`:

```asm
004d9278  CMP [EBX + 0x8], EDI    ; compiler+0x08 (saved category) == param_2?
004d927b  JNZ 0x004d9289
004d927d  CMP [EBX + 0x10], EAX   ; compiler+0x10 (saved ProgId) == param_4?
004d9280  JNZ 0x004d9289
004d9282  MOV ECX, 0x1            ; primary = 1 (this IS the root module)
004d9287  JMP 0x004d928b
004d9289  XOR ECX, ECX            ; primary = 0 (this is a reverse dependent)
```

```c
uVar6 = _VerifyModule_New_20(param_1[6], uVar2, param_4, param_2, param_1);
if (param_2 == (undefined4 *)0x1) {   // if primary-flag was 1
    *param_1 = uVar6;                  //   compiler+0x00 = root result
}
param_1[1] = param_1[1] & uVar6;       // compiler+0x04 &= verify result (aggregate)
```

`_VerifyModule_New@20` (`orlib20.dll:0x10169630`, ordinal 700) compiles a single module via `CELBasicCompiler::CompileText`, returns 1 on success. Root verify is a **full recompile**, not a lightweight check.

### 1.3 Step 2 — Gate: only verify dependents on success, only for categories 0/1/4

```asm
004d92a9  CMP EAX, 0x1          ; verify returned 1?
004d92ac  JNZ 0x004d945f         ; if not, skip to cleanup
004d92b2  CMP EDI, EAX          ; EDI = category. category == 1?
004d92b4  JZ  0x004d92c3        ; yes -> proceed
004d92b6  CMP EDI, 0x4          ; category == 4?
004d92b9  JZ  0x004d92c3        ; yes -> proceed
004d92bb  TEST EDI, EDI          ; category == 0?
004d92bd  JNZ 0x004d945f         ; otherwise skip to cleanup
```

Reverse dependents processed **only when root verify succeeded AND category is 0 (Strategy), 1 (Function), or 4 (SystemFunction)**.

### 1.4 Step 3 — Build the reverse-dependents enumerator

```c
iVar5 = Ordinal_10(0);                         // GetWarehouse(NULL) -> warehouse
if (iVar5 != 0) {
    param_2 = (undefined4 *)0x0;
    iVar5 = FUN_004d7140(param_3, param_4, &param_2);  // build enumerator
    if (iVar5 == 1) { ... }
}
```

`FUN_004d7140` (`0x004D7140`) has two paths:

```c
bool __thiscall FUN_004d7140(int param_1, undefined4 param_2, undefined4 param_3, int *param_4) {
    if (*(int *)(param_1 + 0xc2) == 1) {           // cached/global-lookup path
        Ordinal_71(param_2, param_3);             // InitGlobalLookup
        iVar1 = FUN_004e79d0(param_2, param_3);   // BST lookup by ProgId
        *param_4 = iVar1;
        return *param_4 != 0;
    }
    FUN_004d75b0(param_2, param_3, param_4);      // worker-thread path
    return *param_4 != 0;
}
```

- **Cached path** (`*(warehouse + 0xc2) == 1`): `Ordinal_71` (`InitGlobalLookup`, orlib20 RVA `0x1A9460`) then `FUN_004e79d0` (BST search by ProgId in a per-type tree; falls back to `Ordinal_10` + `FUN_004d75b0` on miss).
- **Worker path** (`FUN_004d75b0`, `0x004D75B0`): sets `*(warehouse + 0x12) = type`, `*(warehouse + 0x16) = ProgId`, `*(warehouse + 4) = 0xd` (**cmd 0x0D = `IWHouse::ProgramById`**), signals worker thread, waits, allocates a `0x241`-byte record via `FUN_004c71f0`, increments `+0x210` refcount.

Output `*param_4` = module record pointer for the just-verified module (the "current record" for contract-change comparison).

### 1.5 Step 4 — Initialize the `CELPrograms` BST and get the dependent count

```c
FUN_004d80d0(local_40);              // construct CELPrograms
iVar5 = FUN_004c9670(local_40);      // initialize (populates BST from warehouse)
if (iVar5 == 1) {
    local_20 = Ordinal_68(local_40);  // count = *(obj + 0x0C)
    if (0 < local_20) { ... loop ... }
}
```

- `FUN_004c9670` (`0x004C9670`) calls `FUN_004cad90` which (under `CSingleLock`) calls `FUN_004d7040` to get a record, then if a SafeArray is present at `*(warehouse + 0x26)`, calls `SafeArrayAccessData` + `FUN_004d88c0` to populate the BST from the SafeArray, then `SafeArrayDestroy`. BST populated from warehouse SafeArray data (binary records, not source text). `FUN_004c9670` reads count internally via `FUN_004d85b0` (`0x004D85B0`: `return *(undefined4 *)(param_1 + 0xc);`) and iterates `0..count-1` calling `FUN_004d85c0` per index.
- `Ordinal_68` (`orlib20.dll:0x101A87D0`, ordinal 68) thunks to `FUN_101e5970` returning `*(param_1 + 0x0C)` — the count field. `FUN_004D91E0` reads count again via this export at `0x004d931c` (loop bound). Both read the same `*(obj + 0x0C)` = **reverse-dependent count**.

### 1.6 Step 5 — Build the "new" record and compare contracts

```c
FUN_004d8fb0(param_3, uVar2);            // build "new" record (calls Ordinal_28 + Ordinal_69)
cVar3 = FUN_004d9040(&local_18);         // compare old vs new; returns 1 if SAME, 0 if CHANGED
local_24 = (uint)(cVar3 == '\0');        // 1 if CHANGED, 0 if no change
```

`FUN_004d8fb0` (`0x004D8FB0`) calls `Ordinal_28` (master-record lookup) + `Ordinal_69` (module-record retrieval) to produce the freshly-verified record. See §2 for comparison.

### 1.7 Step 6 — Per-dependent loop

```c
for (iVar5 = 0; iVar5 < local_20; iVar5 = iVar5 + 1) {
    param_4 = 0;
    iVar7 = FUN_004d85c0(iVar5, &param_4);   // get reverse-dependent record by index
    if (iVar7 == 1) {
        local_2c = param_1[6];                // save compiler+0x18 (HWND)
        if (*(char *)(param_4 + 0x8e) == '\0') {
            iVar7 = FUN_004c9ef0();           // returns *(record + 0x8f)
        } else {
            iVar7 = *(int *)(param_4 + 0x8f);
        }
        param_1[6] = *(uint *)(iVar7 + 0xe4); // compiler+0x18 = dependent's record+0xe4
        iVar7 = FUN_004ca5c0();               // per-dependent "already verified?" check
        if ((iVar7 == 0) || (local_24 == 1)) { // recurse if NOT verified OR contract CHANGED
            ...
            FUN_004d91e0(uVar4);              // RECURSE (0x004D9402)
        }
        param_1[6] = local_2c;                // restore compiler+0x18
        FUN_00410c00();                       // refcount-dec the dependent record
    }
}
```

- `FUN_004d85c0` (`0x004D85C0`): per-index BST accessor. Validates `0 <= index < *(obj + 0x0C)`, walks BST via `FUN_004d7c40` (in-order traversal by index), returns record via `FUN_004d70a0` (cached: `Ordinal_71` + `FUN_004e7840`) or `FUN_004d7190` (worker). Record name at `+0x0e` (or `+0x10` if heap-allocated, gated by `*(uint*)(record+0x24) > 0xf`).
- **Order**: BST traversed in **ascending in-order** (left-root-right) via `FUN_004d7c40` (standard red-black-tree successor walk). Records keyed by ProgId → reverse dependents visited in **ascending ProgId order**.

##### Traversal-order evidence (`FUN_004d7c40`, `FUN_004e79d0`, `FUN_004e7840`)

`FUN_004d7c40` (`0x004D7C40`) — in-order successor walk:

```c
void FUN_004d7c40(int *param_1, int param_2) {
    int *piVar2, *piVar3, *piVar4;
    char cVar1;
    if (0 < param_2) {                       // forward (successor) walk
        piVar4 = (int *)*param_1;            // *param_1 = current node
        do {
            if (*(char *)((int)piVar4 + 0xd) == '\0') {   // not the nil header
                piVar2 = (int *)piVar4[2];                // piVar4[2] = right child
                if (*(char *)((int)piVar2 + 0xd) == '\0') {
                    // right subtree non-empty: go right once, then left to leftmost
                    cVar1 = *(char *)(*piVar2 + 0xd);
                    piVar4 = piVar2;
                    piVar2 = (int *)*piVar2;
                    while (cVar1 == '\0') {
                        cVar1 = *(char *)(*piVar2 + 0xd);
                        piVar4 = piVar2;
                        piVar2 = (int *)*piVar2;
                    }
                    *param_1 = (int)piVar4;
                } else {
                    // no right child: walk up via parent (piVar4[1]) until we are a left child
                    cVar1 = *(char *)(piVar4[1] + 0xd);
                    piVar3 = (int *)piVar4[1];
                    piVar2 = piVar4;
                    while ((piVar4 = piVar3, cVar1 == '\0' && (piVar2 == (int *)piVar4[2]))) {
                        *param_1 = (int)piVar4;
                        cVar1 = *(char *)(piVar4[1] + 0xd);
                        piVar3 = (int *)piVar4[1];
                        piVar2 = piVar4;
                    }
                    *param_1 = (int)piVar4;
                }
            }
            param_2 = param_2 + -1;
        } while (0 < param_2);
    }
    if (param_2 < 0) { /* symmetric predecessor walk (omitted) */ }
    return;
}
```

Node layout (MSVC `std::_Tree`): `piVar4[0]`=`_Left`, `piVar4[1]`=`_Parent`, `piVar4[2]`=`_Right`, byte at `+0xd` (high byte of `piVar4[3]`)=`_Isnil`/color, `piVar4[4]` (offset `0x10`)=key. Textbook BST successor: descend right subtree then leftmost; if no right, walk up parent chain until left child. **Ascending in-order order confirmed.**

`FUN_004e79d0` (`0x004E79D0`, cached BST lookup) — key is ProgId:

```c
int __thiscall FUN_004e79d0(int param_1, undefined4 param_2, int param_3) {
    /* ... EnterCriticalSection(param_1+8) ... */
    FUN_004e7400(&local_1c, &param_2);
    local_18 = (int *)(local_1c + 0x14);
    puVar2 = (undefined4 *)*local_18;        // tree root/header
    cVar1 = *(char *)((int)puVar2[1] + 0xd);
    puVar4 = puVar2;
    puVar3 = (undefined4 *)puVar2[1];
    while (cVar1 == '\0') {
        if ((int)puVar3[4] < param_3) {      // compare node KEY (puVar3[4], +0x10) with param_3
            puVar7 = (undefined4 *)puVar3[2];   // key > param_3 -> go right
            puVar3 = puVar4;
        } else {
            puVar7 = (undefined4 *)*puVar3;     // key <= param_3 -> go left
        }
        puVar4 = puVar3;
        puVar3 = puVar7;
        cVar1 = *(char *)((int)puVar7 + 0xd);
    }
    if ((puVar4 == puVar2) || (param_3 < (int)puVar4[4])) {  // exact match check on puVar4[4]
        puVar4 = puVar2;
    }
    /* ... on miss: Ordinal_10 + FUN_004d75b0 (worker path); on hit: ... */
    local_14 = puVar4[5];    // +0x14 = VALUE (the module-record pointer)
    /* ... LeaveCriticalSection, refcount++ at +0x210 ... */
    return local_14;
}
```

`param_3` = ProgId. BST search compares `puVar3[4]` (node offset `0x10`) against `param_3` → **BST key is ProgId at node offset `0x10`**. Value at node offset `0x14` (`puVar4[5]`) = module-record pointer.

`FUN_004e7840` (`0x004E7840`, per-record cached lookup) confirms same keying:

```c
int __thiscall FUN_004e7840(int param_1, int param_2) {
    /* ... EnterCriticalSection(param_1+8) ... */
    puVar2 = (undefined4 *)*local_14;
    puVar4 = puVar2;
    if (*(char *)((int)puVar2[1] + 0xd) == '\0') {
        puVar3 = puVar2;
        puVar9 = (undefined4 *)puVar2[1];
        do {
            puVar4 = puVar9;
            if ((int)puVar4[4] < *(int *)(param_2 + 0x1e)) {  // node KEY (+0x10) vs record's ProgId (+0x1e)
                puVar9 = (undefined4 *)puVar4[2];             // go right
                puVar4 = puVar3;
            } else {
                puVar9 = (undefined4 *)*puVar4;               // go left
            }
            puVar3 = puVar4;
        } while (*(char *)((int)puVar9 + 0xd) == '\0');
    }
    if ((puVar4 == puVar2) || (*(int *)(param_2 + 0x1e) < (int)puVar4[4])) {
        puVar4 = puVar2;
    }
    /* ... on hit: iVar5 = puVar4[5] (record pointer at +0x14) ... */
}
```

Lookup key is `*(int *)(param_2 + 0x1e)` — the ProgId field of the dependent's record — compared against node key at `puVar4[4]` (offset `0x10`).

- **Per-dependent work**: recursive `FUN_004d91e0` re-enters the whole function, calling `_VerifyModule_New@20` → `CompileText`. Each reverse dependent is **recompiled+verified** (full `CompileText`), not merely checked.

### 1.8 Step 7 — Cleanup

After the loop: `Ordinal_70` releases the enumerator record (`0x004d941b`), `FUN_004d81f0` destroys the `CELPrograms` object, `FUN_004d945f` calls `Ordinal_70` on remaining record. `Ordinal_70` (`orlib20.dll:0x1015DF30`, ordinal 70) = refcount-decrement-and-free (decrements `*(param_1 + 0xca)`, on reaching 1 frees `+0xd2` and the object).

### Answer (R-M.4.1)

- **Traversal**: verify root via `_VerifyModule_New@20` (full recompile). On success, for categories 0/1/4 only, build `CELPrograms` BST of reverse dependents from warehouse (cmd `0x0D` `ProgramById` or cached `InitGlobalLookup` BST), iterate in **ascending ProgId (BST in-order) order**.
- **Per-dependent work**: each reverse dependent **recompiled+verified** via recursive `FUN_004d91e0` → `_VerifyModule_New@20`/`CompileText`.
- **Records**: reverse dependents from TSDev-internal `CELPrograms` BST populated from warehouse SafeArray data (binary master records), keyed by ProgId (node key at offset `0x10`, confirmed by decompilation of `FUN_004e79d0`/`FUN_004e7840`). Warehouse provides data via `GetWarehouse`/`Ordinal_10`, `InitGlobalLookup`/`Ordinal_71`, and worker-thread cmd `0x0D`.

---

## 2. R-M.4.2 — Contract-Change Detection

Contract-change check is `FUN_004d9040` at `TSDev.exe:0x004D9040`, called at `0x004d9346`. Full decompilation:

```c
uint __thiscall FUN_004d9040(int *param_1, int *param_2) {
    iVar3 = *param_1;                          // old record
    if (iVar3 != 0 && (iVar4 = *param_2, iVar4 != 0) &&   // new record
        (*(int **)(iVar3 + 0xd6) == *(int **)(iVar4 + 0xd6))) {  // +0xd6 must match
        bVar5 = *(byte *)(iVar3 + 0x15);
        bVar8 = *(byte *)(iVar4 + 0x15);
        if ((((bVar5 ^ bVar8) & 6) == 0) &&            // +0x15 bits 1,2
            (((bVar5 ^ bVar8) & 0x18) == 0) &&          // +0x15 bits 3,4
            (((bVar5 ^ bVar8) & 0x20) == 0) &&          // +0x15 bit 5
            (((bVar5 ^ bVar8) & 0x40) == 0)) {          // +0x15 bit 6
            uVar1 = *(ushort *)(iVar3 + 0x7f);
            uVar2 = *(ushort *)(iVar4 + 0x7f);
            if ((((uVar1 ^ uVar2) & 7) == 0) &&         // +0x7f bits 0-2
                (((uVar1 ^ uVar2) & 0x1000) == 0) &&    // +0x7f bit 12
                (((uVar1 ^ uVar2) & 0x2000) == 0) &&    // +0x7f bit 13
                (((uVar1 ^ uVar2) & 0x8000) == 0)) {    // +0x7f bit 15
                uVar1 = *(ushort *)(iVar3 + 0x81);
                uVar2 = *(ushort *)(iVar4 + 0x81);
                if ((((uVar1 ^ uVar2) & 0x100) == 0) && // +0x81 bit 8
                    (((uVar1 ^ uVar2) & 0x800) == 0) && // +0x81 bit 11
                    (((uVar1 ^ uVar2) & 0x1000) == 0) &&// +0x81 bit 12
                    (((uVar1 ^ uVar2) & 0x2000) == 0) &&// +0x81 bit 13
                    (((uVar1 ^ uVar2) & 0x4000) == 0) &&// +0x81 bit 14
                    (((uVar1 ^ uVar2) & 0x8000) == 0) && // +0x81 bit 15
                    (*(int *)(iVar3 + 0x29) == *(int *)(iVar4 + 0x29))) {  // +0x29 count
                    iVar11 = 0; iVar10 = 0;
                    while (true) {
                        if (*(int *)(iVar3 + 0x29) <= iVar11) {
                            return 1;                  // ALL MATCH -> "no change"
                        }
                        iVar9 = *(int *)(iVar4 + 0x4f) + iVar10;  // new array elem
                        iVar7 = *(int *)(iVar3 + 0x4f) + iVar10;  // old array elem
                        if ((*(int **)(iVar7 + 0xd) != *(int **)(iVar9 + 0xd)) ||   // elem +0xd
                            (*(int **)(iVar7 + 0x11) != *(int **)(iVar9 + 0x11)) ||  // elem +0x11
                            (*(int **)(iVar7 + 0x15) != *(int **)(iVar9 + 0x15)) ||  // elem +0x15
                            (((*(uint *)(iVar9 + 0x83) ^ *(uint *)(iVar7 + 0x83)) & 4) != 0) ||  // elem +0x83 bit 2
                            (*(short *)(iVar7 + 0x87) != *(short *)(iVar9 + 0x87)) || // elem +0x87
                            (*(int **)(iVar7 + 0xb1) != *(int **)(iVar9 + 0xb1)))) { // elem +0xb1
                            break;                     // MISMATCH -> "changed"
                        }
                        iVar11++; iVar10 += 0xb5;      // stride 0xb5 (181 bytes)
                    }
                    return 0;                          // changed
                }
            }
        }
    }
    return 0;                                          // changed (or null/incompatible)
}
```

**Return convention**: 1 = NO contract change (records contract-equivalent), 0 = changed (or null/incompatible). Caller inverts: `local_24 = (uint)(cVar3 == '\0')` → `local_24 = 1` means **changed**.

#### Fields compared (all warehouse-record metadata, NO source text)

| Offset | Size | Meaning (inferred from comparison) |
|---|---|---|
| `+0xd6` | dword (ptr) | Must be equal between old/new (a type/category pointer) |
| `+0x15` | byte | Bitmask; bits 1,2,3,4,5,6 compared |
| `+0x7f` | ushort | Bitmask; bits 0-2, 12, 13, 15 compared |
| `+0x81` | ushort | Bitmask; bits 8, 11, 12, 13, 14, 15 compared |
| `+0x29` | int | Element count (loop bound) — must be equal |
| `+0x4f` | ptr | Array base; elements compared at stride `0xb5` (181 bytes) |
| elem `+0xd` | dword | Compared for equality |
| elem `+0x11` | dword | Compared for equality |
| elem `+0x15` | dword | Compared for equality |
| elem `+0x83` | uint | Bit 2 compared |
| elem `+0x87` | short | Compared for equality |
| elem `+0xb1` | dword | Compared for equality |

Every compared field is a **fixed-offset numeric/bitmask field inside the warehouse record structure**. No string reads, no source-text pointers, no EasyLanguage parsing. Records obtained from warehouse via `Ordinal_28` and `Ordinal_69` (metadata-record lookups), not from source files.

### Answer (R-M.4.2)

- **Mechanism**: field-by-field metadata comparison of two warehouse module records (old vs new), checking type pointer (`+0xd6`), bitmask fields (`+0x15`, `+0x7f`, `+0x81`), element count (`+0x29`), and per-element comparison of a contract array (`+0x4f`, stride `0xb5`, comparing `+0xd`/`+0x11`/`+0x15`/`+0x83`/`+0x87`/`+0xb1`).
- **No source-text parsing**: confirmed. AGENTS.md-compliant — metadata from orlib20 warehouse records.
- **Return**: 1 = contracts equivalent (no change), 0 = changed. Caller inverts into `local_24` (1 = changed) and gates recursion: skip only when dependent already verified AND contract NOT changed. If contract changed, recursion always proceeds.

---

## 3. R-M.4.3 — Recursion / Cycle Handling

### 3.1 The recursive call

```asm
004d93ff  MOV ECX, EBX            ; this = compiler object (same as parent)
004d9401  PUSH EAX                ; arg = GetSigFromType(record, subtype, ...)
004d9402  CALL 0x004d91e0          ; RECURSE
```

Passes the **same compiler object** (`EBX`) as `this`, with the dependent's category/subtype/ProgId derived from the dependent's record.

### 3.2 No visited-set, no depth counter

- **No visited-set**: no `std::set`, hash table, or per-call bitmap tracking visited ProgIds.
- **No depth counter**: stack frame (`SUB ESP, 0x13c` = 316 bytes) contains only locals for the current iteration; no depth/visited field threaded through the recursive call.
- Only state threaded through recursion is the **compiler object** (`EBX`/`param_1`), whose `+0x18` field is saved/restored around each dependent (`local_2c = param_1[6]` ... `param_1[6] = local_2c`).

### 3.3 Cycle-protection: per-record "verified" flag + contract gate

```c
iVar7 = FUN_004ca5c0();              // check dependent's "verified" flag
if ((iVar7 == 0) || (local_24 == 1)) {   // recurse if NOT verified OR contract CHANGED
    // ... recurse ...
}
```

```asm
004d93a8  CALL 0x004ca5c0       ; iVar7 = verified-flag
004d93ad  TEST EAX, EAX
004d93af  JZ   0x004d93b7       ; if iVar7 == 0 (NOT verified) -> recurse
004d93b1  CMP  [EBP - 0x20], 1  ; local_24 == 1? (contract CHANGED)
004d93b5  JNZ  0x004d9407       ; if verified AND contract NOT changed -> skip recurse
```

- `FUN_004ca5c0` (`0x004CA5C0`) returns `*(byte *)(param_1 + 0x55) & 1` after optional lazy-init via `FUN_004cb430`. Reads **bit 0 of the dependent record's `+0x55` field** — a per-record "already verified" flag.
- `local_24 == 1` means **contract CHANGED**.

**Gate logic**: recursion happens when `iVar7 == 0` (NOT verified) OR `local_24 == 1` (contract CHANGED). Recursion **skipped** only when dependent is already verified AND contract did NOT change. If contract changed, recursion proceeds regardless of verified status.

### 3.4 How cycles are bounded

No explicit cycle guard. Protection is implicit:
1. The `+0x55` "verified" flag is set during `_VerifyModule_New@20`/compile. On a second visit with no contract change, the gate `(verified && NOT changed)` skips recursion.
2. If a true cycle (A→B→A) existed with no contract change, recursion skipped on second visit because dependent already verified AND contract didn't change. Bound is the contract-change check: once a module is verified, its record's contract fields don't change, so `FUN_004d9040` returns "no change" (1), `local_24 = 0`, and the gate skips.

Static analysis cannot fully rule out unbounded recursion in a pathological cycle. The `+0x55` flag and contract gate reduce the cascade but are not a hard visited-set. A dynamic trace with a synthetic cycle would confirm whether the warehouse's reverse-dependent set can ever contain a cycle.

### 3.5 No max depth

No depth limit. Stack overflow bounded only by the 316-byte frame per level and the OS stack.

### Answer (R-M.4.3)

- **Visited-set**: **none.** No visited-set, no depth counter, no per-call bitmap. Only cycle mitigation is a per-record "verified" flag at `*(byte*)(record + 0x55) & 1`, read by `FUN_004ca5c0`.
- **Gate**: recursion skipped only when dependent already verified (`+0x55` bit 0 set) AND contract did NOT change (`FUN_004d9040` returned 1, `local_24 = 0`). If contract changed (`local_24 = 1`), recursion always proceeds regardless of verified status.
- **Max depth**: **none.**
- **Cycle safety**: not guaranteed by an explicit guard. Protection is implicit (verified flag + contract-change gate). Whether the warehouse prevents cycles in the reverse-dependent set is **not proven statically** — see §7.

---

## 4. R-M.4.4 — Failure Reporting

### 4.1 Aggregate result accumulation

```c
uVar6 = _VerifyModule_New_20(param_1[6], uVar2, param_4, param_2, param_1);
if (param_2 == (undefined4 *)0x1) {   // primary-flag was 1 (root)
    *param_1 = uVar6;                  // compiler+0x00 = root result
}
param_1[1] = param_1[1] & uVar6;       // compiler+0x04 &= result (aggregate AND)
```

Applies to **every** verify call (root and recursive). `+0x04` is a running logical AND of all verify results — any failure drives it to 0 permanently. `+0x00` holds the **root** result (only set when primary-flag == 1).

### 4.2 Loop does NOT abort on failure

The per-dependent loop (`0x004d9357`–`0x004d9415`) has no early-exit on verify failure. `INC EDI` at `0x004d9415` continues to the next dependent regardless of returned result.

### 4.3 How the result is consumed

`FUN_00451800` (`TSDev.exe:0x00451800`, UI Verify handler) sets up the compiler object and branches on the root result:

```c
piVar10 = (int *)Ordinal_66();                              // allocate+ctor the compiler object
piVar10[2] = (uint)*(ushort *)(param_1 + 0x1e8);            // compiler+0x08 = category
piVar10[3] = (uint)*(ushort *)(param_1 + 0x1ea);            // compiler+0x0C = subtype
piVar10[4] = (uint)*(ushort *)(param_1 + 0x1ec);            // compiler+0x10 = ProgId
local_14 = piVar10;                                         // save compiler pointer
pHVar11 = CELProperties::GetNotificationWindow(this_01);
piVar10[6] = (int)pHVar11;                                  // compiler+0x18 = HWND
FUN_004d9480();                                             // run verify + reverse-dependent cascade
if (*piVar10 == 1) {                                        // compiler+0x00 (root result) == 1?
    (**(code **)(*(int *)param_1 + 0x1f8))();               // success path (e.g., 0x40A notification)
} else {
    FUN_0044a1e0();                                          // failure path
    ...
}
```

`piVar10 = (int *)Ordinal_66()` (CELCompiler alloc+ctor, orlib20 RVA `0x165F70`) returns pointer to the start of the compiler object. `piVar10[0]` = **`compiler+0x00` (the root result)**. The branch `if (*piVar10 == 1)` reads `+0x00`, not `+0x04`.

**`+0x04` not consumed by the immediate UI path:** After the branch, the compiler pointer is consumed only by:
- success path: virtual call `(**(code **)(*(int *)param_1 + 0x1f8))()` on the **CDocument** (does not receive the compiler pointer, cannot read `+0x04`);
- failure path: `FUN_0044a1e0()` (CELProgramWriter constructor — does not receive the compiler object);
- `FUN_0044c410()` (calls `_GetCompilingErrors_4()`, iterates compiler's error list — does NOT read `+0x04`);
- `FUN_0044b880()` (AfxGetThread message-pump — no compiler access);
- `FUN_004502f0()` (receives CDocument, not compiler);
- `Ordinal_67()` (CELCompiler dtor+free — does not branch on `+0x04`).

None read `compiler+0x04`. The aggregate is computed by `FUN_004D91E0` but is dead with respect to this UI path. (Whether `+0x04` is consumed by any deeper downstream path was not exhaustively traced — see §7.)

`+0x00` is written only by the root verify (primary-flag == 1); recursive calls pass primary-flag 0 and do not write `+0x00`. A dependent failure does NOT overwrite `+0x00`. **The UI reports success/failure based on the root verify result alone**; dependent failures are collected as compiler errors but do not flip the root success flag.

### 4.4 Compiler error collection

`_VerifyModule_New@20` → `CompileText` populates the compiler's errors list. On the failure branch, `FUN_00451800` calls `FUN_0044c410`, which invokes `_GetCompilingErrors_4()` and iterates the error list to surface per-record errors. Per-dependent compile errors are collected and surfaced through `FUN_0044c410`, but the **success/failure decision is the root result**.

### Answer (R-M.4.4)

- **No abort**: per-dependent loop does NOT abort on failure; continues to next dependent.
- **Aggregate accumulation**: `+0x04` is a running AND of all verify results. Any failure drives `+0x04` to 0.
- **Root result drives UI**: `FUN_00451800` checks `compiler+0x00` (root result). `+0x00` only set by root verify (primary-flag == 1). Dependent failure does NOT overwrite `+0x00`. UI reports success/failure based on root verify alone; dependent failures collected as compiler errors.
- **`+0x04` not consumed by immediate UI path**: decompilation of `FUN_00451800` and immediate callees confirms none read `+0x04` after the branch. Aggregate is dead w.r.t. this UI path.
- **Error surfacing**: per-dependent compile errors collected in compiler's error list, displayed through `FUN_0044c410` on failure path. No separate per-dependent error report structure in `FUN_004D91E0` itself.
- **No warning-only path**: function either verifies (1) or not (0); decision is binary on root result.

---

## 5. R-M.4.5 — orlib20 Export Map (historical; superseded by §9)

> **Superseded:** The original Option B2 (reverse-by-inverting-forward) and
> Option B1 (worker-thread coupling) recommendations below are historical.
> §9 (M.4a) resolved the approach: drive cmd `0x0D` then cmd `0x3B` through
> the warehouse worker protocol. The export tables below remain valid
> reference data.

### 5.1 orlib20 export table — dependency APIs

PE export-table scan (`pefile`, ordinal base 10):

| Ordinal | Name | RVA | Direction |
|---:|---|---:|---|
| 487 | `GetDependencies` | `0x153F00` | **forward** (what module depends on) |
| 489 | `_GetDynamicDependencyList@12` | `0x1A9060` | **forward** (dynamic dep list) |
| 531 | `_GetStaticDependencyList@12` | `0x1A8F70` | **forward** (static dep list) |
| 396/397 | `_DeleteChangedModulesList@4` / `_DeleteDependencyList@4` | `0x1A9150` | free a dep list |

`GetDependencies` decompilation (`0x10153F00`):

```c
undefined4 GetDependencies(int param_1, undefined4 param_2, undefined4 param_3, undefined4 *param_4) {
    iVar2 = FUN_101a9650(0);                      // GetWarehouse
    if (param_1 != 0x32) {
        FUN_101d43a0(param_1, local_40);          // expand type
        iVar2 = FUN_101cff60(local_40, param_2, local_14c);  // master record lookup
        if ((iVar2 != 0) && ((local_d3 & 4) != 0)) {
            FUN_101d4280(param_1, local_64 >> 8, local_14c, 1);
            uVar3 = FUN_101a39e0();
            *param_4 = 0;
            uVar4 = FUN_10153ad0(param_4);        // build dep list
            FUN_1015df30(uVar3, 0);                // release
        }
    }
    return uVar4;
}
```

`GetDependencies` returns **forward** dependency list, gated on `local_d3 & 4`. `_GetDynamicDependencyList@12` and `_GetStaticDependencyList@12` walk `DAT_1023dccc` (global tree) building arrays of `{param_2, param_3, ...}` entries — also **forward**. **No orlib20 export performs reverse-dependent enumeration or revalidation.**

### 5.2 orlib20 primitives TSDev uses (all available to the CLI)

| Primitive | orlib20 export | RVA | CLI already uses? |
|---|---|---:|---|
| Warehouse singleton | `Ordinal_10` (GetWarehouse, no name) | `0x1A9650` | Yes (delete orchestrator) |
| Master-record lookup by type/name | `Ordinal_28` (no name) | `0x1A03F0` | Not directly (CLI uses `_GetMasterRecByType@16`) |
| Module-record retrieval | `Ordinal_69` (no name) | `0x1A3E00` | No |
| Count accessor (`*(obj+0x0C)`) | `Ordinal_68` (no name) | `0x1A87D0` | No |
| Refcount-dec / free | `Ordinal_70` (no name) | `0x15DF30` | No |
| Global lookup init | `Ordinal_71` (InitGlobalLookup, no name) | `0x1A9460` | Yes (signature target) |
| Verify (compile) | `_VerifyModule_New@20` (ordinal 700) | `0x169630` | **Yes (core CLI verify)** |
| CELCompiler alloc+ctor | `Ordinal_66` (no name) | `0x165F70` | Yes (M.1) |
| CELCompiler dtor+free | `Ordinal_67` (no name) | `0x165FD0` | Yes (M.1) |
| Type→power-type expansion | `FUN_101d43a0` (internal) | `0x1D43A0` | No (CLI has its own type map) |

All ordinals 10, 28, 66, 67, 68, 69, 70, 71 are **ordinal-only `[NONAME]` exports** (confirmed via `pefile`), resolvable by `GetProcAddress(hModule, MAKEINTRESOURCEA(ordinal))`.

### 5.3–5.4 Historical recommendations (superseded — see §9)

The original note proposed **Option B2** (reverse-by-inverting-forward using exported APIs: `_LoadMasterRecsEX@16` ord 568, `_GetMasterRecsBuf@12` ord 512, `_GetMasterRecByType@16` ord 510, `GetDependencies` ord 487, `_GetStaticDependencyList@12` ord 531, `_DeleteDependencyList@4` ord 397, `_VerifyModule_New@20` ord 700 — all named exports) and rejected **Option B1** (driving the worker thread / `CELPrograms` BST) as high-risk coupling. Both are superseded by §9: the warehouse provides reverse dependents directly via cmd `0x3B` (`IProgram::Callers`), which IS the worker-thread approach (Option B1) that the original note rejected — the "high-risk coupling" concern was overstated since the CLI already drives cmd `0x0D` for `exists`.

### Historical answer (R-M.4.5; superseded by §9)

- **(a)** No orlib20 export provides reverse-dependent enumeration/revalidation. Forward dependency exports only. `GetDependencies` is broken (§8.1: checks `0xFA4`, compiler writes `0xFA3`).
- **(b)** Historical composition: `_VerifyModule_New@20` + `GetDependencies`/`_GetStaticDependencyList@12` (forward deps, to invert) + `_GetMasterRecByType@16` (record reads) + `_DeleteDependencyList@4` (cleanup).
- **(c)** Historical recommendation: Option B2 (reverse-by-inverting-forward). Superseded by §9 warehouse-level approach.

---

## 6. Historical Synthesis (superseded by §9)

The original plan proposed Option B2 (reverse-by-inverting-forward) with 7 steps: root verify via `_VerifyModule_New@20`, reverse-dependent enumeration via `_LoadMasterRecsEX@16`/`_GetMasterRecsBuf@12` + `GetDependencies`/`_GetStaticDependencyList@12` filtering, contract-change check via `_GetMasterRecByType@16` comparing §2 field offsets, per-dependent recompile, explicit visited-set for cycle safety, AND-aggregate failure handling with root result driving UI, and category gating (0/1/4 only). **All superseded by §9.**

**Key parity note:** Reverse-dependent revalidation is a **parity** feature (M.4), not a linkability fix (Workstream L). The root verify (`_VerifyModule_New@20`) is the linkability-critical step; the reverse-dependent cascade is a propagation step for already-running consumers.

---

## 7. Gaps / Open Questions

1. **Cycle safety — dynamic confirmation needed.** No explicit visited-set; cycle protection is implicit (`+0x55` flag + contract-change gate). Whether the warehouse can contain a cycle in the reverse-dependent set is not proven statically. CLI replication should use an explicit visited-set (strictly safer).

2. **`+0x18` field overload.** `FUN_004D91E0` temporarily overwrites `compiler+0x18` (HWND) with `*(record + 0xe4)` during the reverse-dependent loop. Does not affect CLI replication (which uses its own HWND).

3. **`CELPrograms` BST population source — RESOLVED by §9 (M.4a).** SafeArray at `*(warehouse + 0x26)`, produced by cmd `0x3B` (`IProgram::Callers`). Cmd `0x0D` retrieves the root module's single record; cmd `0x3B` returns the reverse-dependent set. See §9.1–§9.3.

4. **Contract-change field semantics.** Exact semantic meaning of each compared field in `FUN_004d9040` is not labeled in the binary. Offsets and comparison logic are exact; field names are inferred. CLI can compare the same offsets without knowing semantic names.

5. **No TSDev UI reverse-traversal trace captured for original M.4 slice.** A Frida trace of a UI Verify triggering reverse-dependent revalidation would confirm recursion order, `+0x55` flag behavior, `+0x18` overload, and failure-reporting aggregate. (Partially addressed by §9.8 session 3 Frida trace.)

6. **`compiler+0x04` downstream consumption not exhaustively traced.** §4.3 confirms none of `FUN_00451800`'s immediate callees read `+0x04` after the branch. Whether deeper downstream paths consume it was not traced. Does not affect the M.4.4 conclusion (UI branch is provably on `+0x00`).

---

## Evidence Index

### TSDev.exe (image base `0x00400000`)

| Address | Symbol / Role | Cited in |
|---|---|---|
| `0x004D91E0` | `FUN_004D91E0` — reverse-dependent reverify wrapper (subject of M.4) | §1, §3, §4 |
| `0x004D9480` | `FUN_004D9480` — caller; zeros `+0x00`/`+0x04`, calls `FUN_004D91E0(category, subtype, ProgId)` | §1.1 |
| `0x00451800` | `FUN_00451800` — UI Verify handler; sets compiler `+0x08`/`+0x0C`/`+0x10`/`+0x18`, checks `+0x00` for success | §1.1, §4.3 |
| `0x004D9402` | recursive `CALL 0x004d91e0` | §1.7, §3.1 |
| `0x004D9299` | `CALL 0x0053f5bc` (`_VerifyModule_New@20` thunk) | §1.2 |
| `0x004d9278`–`0x004d928b` | primary-flag computation (`compiler+0x08`/`+0x10` vs `param_2`/`param_4`) | §1.2 |
| `0x004d92a9`–`0x004d92bd` | category gate (0/1/4 only) | §1.3 |
| `0x004D7140` | `FUN_004d7140` — reverse-dependents enumerator builder (cached: `Ordinal_71`+`FUN_004e79d0`; worker: `FUN_004d75b0`) | §1.4 |
| `0x004D75B0` | `FUN_004d75b0` — worker-thread path; command `0x0D` (`ProgramById`) | §1.4 |
| `0x004D80D0` | `FUN_004d80d0` — `CELPrograms` constructor (sets vftable, builds BST via `FUN_004d8a90`) | §1.5 |
| `0x004C9670` | `FUN_004c9670` — `CELPrograms` init (populates BST from SafeArray via `FUN_004cad90`) | §1.5 |
| `0x004CAD90` | `FUN_004cad90` — SafeArray→BST population | §1.5 |
| `0x004D85B0` | `FUN_004d85b0` — count accessor (`return *(obj+0x0C)`); called by `FUN_004c9670` at `0x004c9699` | §1.5 |
| `0x004D85C0` | `FUN_004d85c0` — per-index BST record accessor | §1.7 |
| `0x004D7C40` | `FUN_004d7c40` — BST in-order (successor/predecessor) traversal by index | §1.7 |
| `0x004E7840` | `FUN_004e7840` — cached per-record BST lookup keyed by record ProgId (`+0x1e`) | §1.7 |
| `0x004E79D0` | `FUN_004e79d0` — cached BST lookup keyed by ProgId (node key at `+0x10`) | §1.4, §1.7 |
| `0x004D8FB0` | `FUN_004d8fb0` — builds "new" record (`Ordinal_28`+`Ordinal_69`) | §1.6 |
| `0x004D9040` | `FUN_004d9040` — contract-change comparison (metadata-only) | §2 |
| `0x004CA5C0` | `FUN_004ca5c0` — per-record "verified" flag (`*(byte*)(rec+0x55) & 1`) | §3.3 |
| `0x004C9EF0` | `FUN_004c9ef0` — returns `*(record + 0x8f)` | §1.7 |
| `0x004C9900` | `FUN_004c9900` — locked record accessor | §1.7 |
| `0x004CA510` | `FUN_004ca510` — locked record accessor | §1.7 |
| `0x00410C00` | `FUN_00410c00` — refcount-dec (`*(p+0x84)`) | §1.7 |
| `0x004D81F0` | `FUN_004d81f0` — `CELPrograms` destructor | §1.8 |
| `0x004D945F` | `FUN_004d945f` — cleanup (calls `Ordinal_70`) | §1.8 |
| `0x004D7A30` | `FUN_004d7a30` — category→power-type expansion | §1.1 |
| `0x0044C410` | `FUN_0044c410` — failure-path error-list display (`_GetCompilingErrors_4`); does not read `compiler+0x04` | §4.3 |
| `0x0044A1E0` | `FUN_0044a1e0` — `CELProgramWriter` constructor (failure branch; not the error-display routine) | §4.3 |
| `0x0044B880` | `FUN_0044b880` — `AfxGetThread` message-pump (post-branch; no compiler access) | §4.3 |
| `0x004502F0` | `FUN_004502f0` — post-verify CDocument/CELProperties reload (receives CDocument, not compiler) | §4.3 |
| `0x0053f5bc` | thunk → `orlib20!_VerifyModule_New@20` | §1.2 |
| `0x0053f550` | thunk → `orlib20!Ordinal_28` | §1.2 (disasm) |
| `0x0053f59e` | thunk → `orlib20!Ordinal_69` | §1.2 (disasm) |
| `0x0053f562` | thunk → `orlib20!GetSigFromType` | §1.7 |
| `0x0053f55c` | thunk → `orlib20!Ordinal_10` (GetWarehouse) | §1.4 |
| `0x0053f5b6` | thunk → `orlib20!Ordinal_68` | §1.5 |

### orlib20.dll (image base `0x10000000`, baseline `9.50.01.3344`)

| Address | Ordinal | Name (if any) | Role | Cited in |
|---|---:|---|---|---|
| `0x10169630` | 700 | `_VerifyModule_New@20` | verify (CompileText) — no reverse-dep traversal inside | §1.2, §5.2 |
| `0x101A9650` | 10 | (none) | `GetWarehouse` — warehouse singleton | §5.2 |
| `0x101A03F0` | 28 | (none) | master-record lookup by type/name | §5.2 |
| `0x101A87D0` | 68 | (none) | count accessor (`*(obj+0x0C)`) — thunk to `FUN_101e5970` | §1.5, §5.2 |
| `0x101E5970` | — | (internal) | body of Ordinal_68 | §1.5 |
| `0x101A3E00` | 69 | (none) | module-record retrieval | §5.2 |
| `0x1015DF30` | 70 | (none) | refcount-dec/free | §1.8, §5.2 |
| `0x101A9460` | 71 | (none) | `InitGlobalLookup` | §5.2 |
| `0x10165F70` | 66 | (none) | CELCompiler alloc+ctor (M.1) | §5.2 |
| `0x10165FD0` | 67 | (none) | CELCompiler dtor+free (M.1) | §5.2 |
| `0x1019FDF0` | 530 | `GetSigFromType` | identity function (returns arg) | §1.7 |
| `0x10153F00` | 487 | `GetDependencies` | **forward** dependency list | §5.1 |
| `0x101A9060` | 489 | `_GetDynamicDependencyList@12` | **forward** dynamic dep list | §5.1 |
| `0x101A8F70` | 531 | `_GetStaticDependencyList@12` | **forward** static dep list | §5.1 |
| `0x101A9150` | 396/397 | `_DeleteChangedModulesList@4` / `_DeleteDependencyList@4` | free dep list | §5.1 |
| `0x101A0460` | 510 | `_GetMasterRecByType@16` | master-record lookup by type (named export) — mentioned in the historical Option B2 / §6 analysis | §5.4(c), §6 |
| `0x101A5360` | 568 | `_LoadMasterRecsEX@16` | enumerate master records of a type (named export) — used by `exists`; mentioned in the historical Option B2 / §6 analysis | §5.4(c), §6 |
| `0x101A0C70` | 512 | `_GetMasterRecsBuf@12` | master-records buffer accessor (named export) — used by `exists`; mentioned in the historical Option B2 / §6 analysis | §5.4(c), §6 |
| `0x101CFF60` | — | (internal) | `FUN_101cff60` — master-record lookup (used by Ordinal_28, _VerifyModule_New) | §1.4 |
| `0x101D43A0` | — | (internal) | type→power-type expansion | §5.2 |

### PE export-table evidence

- `pefile` scan: ordinal base = 10; ordinals 10, 28, 66, 67, 68, 69, 70, 71 are all `[NONAME]` (ordinal-only).
- Named exports: ord 510 `_GetMasterRecByType@16` (RVA `0x1A0460`), ord 568 `_LoadMasterRecsEX@16` (RVA `0x1A5360`), ord 512 `_GetMasterRecsBuf@12` (RVA `0x1A0C70`). All resolvable by `GetProcAddress(hModule, "<name>")`.

### Prior RE cited

- `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md` §6 (Verify chain static evidence)
- `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md` §F (reverse-dependent revalidation not required for root linkability)
- `docs/RE/RE_M3_0x40A_NOTIFICATION.md` (format pattern + GetWarehouse RVA cross-reference)
- `docs/M1_HANDOFF.md` §R-M.1.2 (compiler field layout)
- `docs/SIGNATURE_CATALOG.md` §4.7, §5 (GetWarehouse RVA, ordinals 66/67 resolution)

---

## 8. GetDependencies Mismatch & Linker-Based Dep-List Approach (superseded by §9)

> **Superseded by §9 (M.4a):** The linker-based approach below parses orlib20
> internal buffer layouts that TSDev.exe never touches directly. M.4b replaces
> it with the warehouse-level cmd `0x0D`/`0x3B` approach. Retained as RE
> reference for the linker/internal-buffer layout.

### 8.1 GetDependencies is broken: type-code mismatch (0xFA4 vs 0xFA3)

`FUN_10153ad0` checks for dep-list entry type `0xFA4`, but `MakeSymbol` (RVA `0x10021800`) writes `0xFA3`. Confirmed by runtime diagnostic: 7 `0xFA3` entries in 2 Indicators, zero `0xFA4` across all 25 tested items. GetDependencies returns empty for ALL items.

### 8.2 record+0x6f is NOT a dependency list

`record+0x6f` (size at `record+0x73`) is a **64KB compilation working buffer**, not a dep-list. Confirmed by `GetTokenOnlyInfo` (`0x1001f310`), `FUN_10151550` (record destructor), and `FUN_101158d0` (LinkModules) — all read it as compiled tokens.

### 8.3 Actual dep-lists are at element+0x7b (per-element)

- `element+0x7b` = dep-list buffer pointer
- `element+0x73` = dep-list byte size
- Element stride `0xb5` (181 bytes). Element array at `record+0x4f` (pointer), count at `record+0x29`.

### 8.4 Dep-list entry format

- `+0x00` (4 bytes): type (dword) — e.g., `0xFA3` for function dependency
- `+0x04` (1 byte): total entry length (iteration stride)
- `+0x05` (4 bytes): source line
- `+0x09` (4 bytes): source column
- `+0x0d` (4 bytes): payload — for `0xFA3`/`0xFA4` entries, the **dependent Function's ProgId** (int)

Entry iteration: advance by length byte at `entry+4`. Continue while `(cursor - base) < depListSize`.

### 8.5 entry+0xd IS a ProgId

`MakeSymbol` writes `(int)(param_9 + 0.5)` (float-to-int round) at `entry+0xd`. Next line passes it to `GetFunctionReturnType(1, progId)`, which resolves via BST lookup keyed by ProgId. `ExpandFuncList` (`0x1001c890`) is a trivial dedup-collect utility — no resolution needed; `entry+0xd` can be directly compared to the modified Function's ProgId.

### 8.6 Category-specific dep-list presence

- **Strategies (cat 0):** dep-lists exist (size=38, 2 entries/element). Tested items had only `0xBB8`/`0xBB9` entries (no function calls).
- **Indicators (cat 2):** dep-lists exist (size 35/38/80). `0xFA3` entries in items calling Functions.
- **Functions (cat 4):** dep-lists empty (size=0, ptr=NULL) in sampled existing Functions.

### 8.7 Linker-based approach (superseded by §9)

Enumerate reverse dependents by: (1) call linker (ordinal 85) per candidate, (2) read element array (`record+0x4f`/`+0x29`, stride `0xb5`), (3) read dep-list (`element+0x7b`/`+0x73`), (4) parse entries checking type `0xFA3` AND `0xFA4` (ProgId at `entry+0xd`), (5) match against `rootProgId`. Linker call requires partial record buffer (via `Ordinal_28`), release via `Ordinal_70`.

### 8.8 Known limitation (corrected per M.3b.2c)

Original claim that CLI-compiled items have empty element arrays was a **test artifact** — declaration-free source (`X = 1;`) legitimately has `elemCount=0`. M.3b.2c verified CLI-compiled items WITH declarations have populated element arrays across all 5 types (Strategy/Indicator/Function/TradingApp/ShowMe).

**Remaining linker limitation:** Linker (ordinal 85) and `Ordinal_28` take the EXPANDED whType (enum value: 0, 2, 4, 5122, 1282), NOT the category (low 8 bits). For TradingApp (5122) and ShowMe (1282), passing category=2 causes NULL return. This is bypassed by M.4b's warehouse-level approach.

---

## 9. M.4a Findings — Warehouse-Level `CELPrograms` BST Population

> **Date:** 2026-08-28. **Method:** Ghidra static analysis of `TSDev.exe`,
> `orlib20.dll`, `WHServer.exe`. **Scope:** Research-only.

### 9.0 Architecture overview

The reverse-dependent enumeration is a **warehouse COM operation**, not a linker/internal-buffer operation. Full call chain:

```
FUN_004D91E0 (TSDev.exe, reverse-dependent reverify wrapper)
  └─ FUN_004c9670 (TSDev.exe, init CELPrograms BST)           [§1.5]
       └─ FUN_004cad90 (TSDev.exe, populate BST from SafeArray)
            ├─ FUN_004d7040 (TSDev.exe, cmd 0x0D requester)   → root record
            ├─ *(warehouse+0x5e) = root record                 → set current record
            ├─ FUN_004d6f00(0x3B) (TSDev.exe, cmd 0x3B requester)
            │    └─ [orlib20 worker thread] FUN_101d2150 case 0x3B
            │         └─ (*IProgram_vtable[+0x38])(pProgram)   → IProgram::Callers
            ├─ SafeArrayAccessData(*(warehouse+0x26))          → SafeArray of callers
            ├─ FUN_004d88c0 (parse SafeArray entries → BST)
            └─ SafeArrayDestroy
```

> **warehouse vs. record:** The cmd `0x3B` result SafeArray is at **`warehouse+0x26`** in the `CELWHouse` singleton, NOT in the `CELProgram` record. The CELProgram record (the "current record" for `IProgram::Callers`) is a separate object at **`warehouse+0x5e`** with its own metadata fields (`+0xd6`, `+0x15`, `+0x29`, `+0x4f`; used by `FUN_004d9040`; see §9.6).

The warehouse is a **COM STA object**. `CELWHouse` class (`FUN_101cf1f0`, orlib20 RVA `0x1cf1f0`) creates completion event (`+0x6a`), command-ready event (`+0x6e`), mutex (`+0x72`), and starts a **worker thread** via `_beginthreadex(FUN_101d4110, ...)` in `FUN_101d1310` (RVA `0x1d1310`). Worker thread (`FUN_101d4110` → `FUN_101d2150`, RVA `0x1d2150`) calls `CoInitialize`, creates `WHServer` COM object via `CoCreateInstance` (CLSID `CLSID_WHouse`, IID `IID_IWHouse`), enters command-dispatch loop: waits on `+0x6e`, switches on `*(warehouse+0x04)` (command), calls corresponding `IWHouse`/`IProgram` vtable method, stores result, signals `+0x6a`.

`CELWHouse` constructor sets `*(warehouse+0x76) = GetCurrentThreadId()` (owning thread). Generic command requester `FUN_101cf5b0` (RVA `0x1cf5b0`): acquires mutex `+0x72`, checks `GetCurrentThreadId() == *(warehouse+0x76)` (asserts `int 3` on mismatch, except cmd `0x1e`), sets `*(warehouse+0x04) = cmd`, signals `+0x6e`, waits on `+0x6a`. TSDev-side equivalent: `FUN_004d6f00` (`0x004d6f00`), same protocol.

`WHServer.exe` COM server implements three dual `IDispatch` interfaces (confirmed via RTTI):
- **`IWHouse`** (class `CWHouse`, CLSID `CLSID_WHouse`, IID `IID_IWHouse`) — warehouse server.
- **`IProgram`** (class `CProgram`, CLSID `CLSID_Program`, IID `IID_IProgram`) — per-program object.
- **`ICategory`** (class `CCategory`, IID `IID_ICategory`).

`IProgram` exposes **`Callers`** (reverse dependents) and **`Callees`** (forward dependents), confirmed by ASCII strings at `WHServer.exe:0x004318e8` ("Callers") and `0x004318f0` ("Callees"), referenced from type-library descriptor at `0x0043e06c`/`0x0043e070`.

### 9.1 R-M.4a.1 — cmd 0x0D retrieves the ROOT module's single record

`FUN_004d7040` retrieves the ROOT module's single `CELProgram` record via cmd `0x0D` (`ProgramById`). It does NOT retrieve reverse dependents.

Evidence (`FUN_004cad90`, `0x004cad90`):

```c
if (*(int *)(param_1 + 0x10) == 0)
    *(int *)(param_1 + 0x10) =
        FUN_004d7040((int)*(short *)(param_1 + 0x15),     // type
                      *(undefined4 *)(param_1 + 0x18));    // ProgId
*(undefined4 *)(*(int *)(param_1 + 0xc) + 0x5e) =
    *(undefined4 *)(param_1 + 0x10);                       // warehouse+0x5e = root record
FUN_004d6f00(0x3b);                                        // issue cmd 0x3B
```

`FUN_004d7040` (`0x004d7040`) performs cmd `0x0D`:

```c
*(undefined4 *)(param_1 + 0x12) = param_2;   // warehouse+0x12 = type
*(undefined4 *)(param_1 + 0x16) = param_3;   // warehouse+0x16 = ProgId
*(undefined4 *)(param_1 + 4)    = 0xd;       // warehouse+0x04 = cmd 0x0D
SetEvent(*(HANDLE *)(param_1 + 0x6e));
WaitForSingleObject(*(HANDLE *)(param_1 + 0x6a), 0xffffffff);
return *(undefined4 *)(param_1 + 0x5e);      // result = single record
```

Type/ProgId from `CELPrograms` object's `+0x15` (type) and `+0x18` (ProgId) = the **root module**. orlib20 worker handler for cmd `0x0D` (`FUN_101d15c0`, RVA `0x1d15c0`) accepts type + ProgId, allocates a single `CELProgram` object, returns single record. `FUN_101d04f0` (RVA `0x1d04f0`) and `FUN_101e3990` (cached BST lookup by ProgId) confirm both paths return a single record by type/ProgId.

`FUN_004d7040` enforces thread-affinity check `GetCurrentThreadId() == *(warehouse+0x76)` (asserts `int 3` on mismatch). **M.4b must issue both cmd `0x0D` and cmd `0x3B` from the warehouse-owning thread.**

### 9.2 R-M.4a.2 — cmd 0x3B populates the SafeArray at warehouse+0x26

cmd `0x3B` dispatches to `IProgram` COM vtable method at offset `+0x38` (the `Callers` method). Operates on the "current record" (root program, set at `warehouse+0x5e` before the command). Result SafeArray stored at **`warehouse+0x26`**.

Worker thread dispatch (`FUN_101d2150`, RVA `0x1d2150`), case `0x3b`:

```c
case 0x3b:
    piVar9 = *(int **)(...);                          // the IProgram COM object
    uVar11 = (**(code **)(*piVar9 + 0x38))(piVar9);   // vtable[+0x38](this)
    *(undefined4 *)(...) = uVar11;                    // store result
    break;
```

Sibling commands → `IProgram` vtable methods:
- **cmd `0x39`** → vtable `+0x54` (save/modify path, reads string list via `CMemFile`).
- **cmd `0x3A`** → vtable `+0x40` (UI list path, builds "Name"/"Category" strings — likely "Callees"/forward-dependents view).
- **cmd `0x3B`** → vtable `+0x38` (reverse-dependent revalidation path).

All three take only the COM `this` pointer (no type/ProgId args). Operate on current record at `*(warehouse+0x5e)`. Result SafeArray read by `FUN_004cad90` from `*(warehouse + 0x26)`:

```c
// FUN_004cad90, after FUN_004d6f00(0x3b):
pvVar = SafeArrayAccessData(*(SAFEARRAY **)(warehouse + 0x26));
FUN_004d88c0(..., pvVar, count);    // populate BST from SafeArray
SafeArrayDestroy(...);
```

`FUN_004cad90` first checks `*(warehouse + 0x4e) == 0` (status/error) and calls `FUN_004c9270` (uses `SafeArrayGetLBound`/`SafeArrayGetUBound`) to obtain element count before `SafeArrayAccessData`. **M.4b should replicate this guard.**

### 9.3 R-M.4a.3 — SafeArray contains reverse dependents DIRECTLY

The warehouse supplies reverse dependents DIRECTLY. TSDev does NOT enumerate all programs and filter. The SafeArray from cmd `0x3B` (`IProgram::Callers`) contains exactly the reverse dependents (callers) of the current (root) program.

Evidence:
1. **Semantic naming:** `IProgram` exposes "Callers" (reverse dependents) and "Callees" (forward dependents) — standard terms.
2. **Exclusive use:** cmd `0x3B` called by exactly one TSDev.exe function: `FUN_004cad90` (sole caller: `FUN_004c9670` at `0x004c967b`). `FUN_004c9670` called by `FUN_004D91E0` (`0x004d930a`) and `FUN_004c9710`. On orlib20 side, cmd `0x3B` issuer (`FUN_101c51b0`) also reached from modify/rename flows — "Callers" is a general reverse-dependent query.
3. **No filtering logic:** `FUN_004cad90` reads SafeArray and passes ALL entries to `FUN_004d88c0` for BST insertion. `FUN_004d88c0` iterates every entry (variable stride `8 + nameLength + 0x16`) and inserts each into BST via `FUN_004d7f40`. No predicate/filter — every SafeArray entry becomes a BST node.
4. **Current-record context:** vtable `+0x38` takes no type/ProgId arguments. A "callers of this program" query needs only the current program context; a "all programs of a type" query would need a type argument.

**Conclusion:** The warehouse COM server (`WHServer.exe` `CProgram`/
`IProgram`) directly computes and returns the reverse-dependent (Callers) set.
TSDev consumes it as-is. The CLI should rely on this same mechanism rather
than enumerating all programs and filtering, or parsing linker dep-lists.

### 9.4 R-M.4a.4 — CLI command sequence

The CLI should reproduce the TSDev command sequence:

1. **Get warehouse** via `Ordinal_10` (GetWarehouse, RVA `0x1a9650`).
2. **Retrieve root program record** via cmd `0x0D`: set `*(warehouse+0x12) = type`, `*(warehouse+0x16) = ProgId`, `*(warehouse+0x04) = 0x0D`, signal `+0x6e`, wait on `+0x6a`, read result from `*(warehouse+0x5e)`. (orlib20 wrappers: `FUN_101d15c0` RVA `0x1d15c0` / `FUN_101d04f0` RVA `0x1d04f0`. CLI already drives cmd `0x0D` indirectly via `_GetMasterRecByType@16` for `exists`.)
3. **Set current record:** `*(warehouse+0x5e) = root_record` (done internally by `FUN_004cad90` before cmd `0x3B`).
4. **Issue cmd `0x3B` (Callers):** set `*(warehouse+0x04) = 0x3B`, signal `+0x6e`, wait on `+0x6a`. (orlib20 requester: `FUN_101cf5b0(0x3b)` RVA `0x1cf5b0`; TSDev-side: `FUN_004d6f00(0x3b)`.)
5. **Guard, count, read SafeArray:** verify `*(warehouse+0x4e) == 0`, get element count via `SafeArrayGetLBound`/`SafeArrayGetUBound` (as `FUN_004c9270` does), read data from `*(warehouse+0x26)`.
6. **Parse SafeArray entries** using `FUN_004d88c0` format (§9.7), build dependent set.
7. **For each dependent,** recurse verify + contract-change gate as `FUN_004D91E0` does (§1.6–1.7, §2, §3).

**Key caveat — cmd `0x3B` requester is NOT exported.** `FUN_101cf5b0` and `FUN_004d6f00` are internal/unexported. The CLI must drive the warehouse worker protocol directly (option (a)): write `*(warehouse+0x04)`, signal `+0x6e`, wait on `+0x6a` using the warehouse pointer from `Ordinal_10` — replicating `FUN_101cf5b0`'s protocol. Requires warehouse object layout offsets (`+0x04`, `+0x5e`, `+0x6a`, `+0x6e`, `+0x72`, `+0x76`), all documented in this note. No exported orlib20 wrapper for cmd `0x3B` was found (§9.5).

### 9.5 R-M.4a.5 — Exported orlib20 APIs

No exported orlib20 API provides pure reverse-dependent enumeration without side effects. cmd `0x3B` requester (`FUN_101cf5b0`) is internal/unexported.

`_ChangeModuleFileName@20` (`0x1015e450`) transitively issues cmd `0x3B` but is a file-rename operation with side effects — NOT a clean enumeration primitive. Internal higher-level orchestrators (`FUN_101a4df0`, `FUN_101a50a0`, `FUN_101a4f90`) wrap the full `cmd 0x0D` → `cmd 0x3B` flow but are not exported.

Required primitives:

| Primitive | Role | Ordinal | orlib20 RVA | Exported? |
|-----------|------|---------|-------------|-----------|
| `GetWarehouse` | Warehouse singleton | 10 | `0x1a9650` | Yes |
| `InitGlobalLookup` | Cached BST init | 71 | `0x1a9460` | Yes |
| `_GetMasterRecByType@16` | Master-record lookup by type/name | 28 | `0x1a03f0` | Yes |
| (module-record retrieval) | Module record from master | 69 | `0x1a3e00` | Yes |
| (count accessor) | `*(obj+0x0C)` count | 68 | `0x1a87d0` | Yes |
| (refcount-dec/free) | Release record | 70 | `0x15df30` | Yes |
| `_VerifyModule_New@20` | Verify/recompile single module | 700 | `0x169630` | Yes |
| cmd `0x0D` requester | ProgramById (single record) | — | `0x1d15c0` (`FUN_101d15c0`) | No (internal) |
| cmd `0x0D` dispatcher | Cached/worker dispatch | — | `0x1d04f0` (`FUN_101d04f0`) | No (internal) |
| **cmd `0x3B` requester** | **Callers (reverse dependents)** | — | `0x1cf5b0` (`FUN_101cf5b0`) | **No (internal)** |
| cmd `0x3B` worker handler | Dispatch to IProgram::Callers | — | `0x1d2150` case 0x3b | No (internal) |
| BST insert (SafeArray→BST) | Parse SafeArray entries | — | TSDev `0x004d88c0` | No (TSDev-private) |

Forward-dependency exports (`GetDependencies` ord 487, `_GetDynamicDependencyList@12` ord 489, `_GetStaticDependencyList@12` ord 531) return Callees, not Callers, and `GetDependencies` is broken (§8.1). **CLI must drive the warehouse worker command protocol directly.**

### 9.6 R-M.4a.6 — Contract-change comparison (warehouse-metadata, no source-text)

Confirmed: `FUN_004d9040` (`0x004d9040`) compares warehouse-record metadata fields between old and new root records. NO source-text comparison, NO EasyLanguage parsing. AGENTS.md-compliant.

Returns `1` if UNCHANGED, `0` if CHANGED. Exact field offsets and comparison masks (duplicates §2 table — see §2 for full decompilation):

| Record offset | Size | Comparison | Meaning |
|---------------|------|------------|---------|
| `+0x00` | 4 | `*rec != 0` (both must be non-NULL) | record validity |
| `+0xd6` | 4 | `old[+0xd6] == new[+0xd6]` | identity/version (must match) |
| `+0x15` | 1 | `(old ^ new) & 0x7E == 0` | flags: bits 1–6 |
| `+0x7f` | 2 | `(old ^ new) & 0xB007 == 0` | flags: low byte bits 0–2, plus bits 12,13,15 |
| `+0x81` | 2 | `(old ^ new) & 0xF900 == 0` | flags: bits 8,11,12,13,14,15 |
| `+0x29` | 4 | `old[+0x29] == new[+0x29]` | element count (must match) |
| per-element (stride `0xb5`, base `*(rec+0x4f)`): | | | |
| `elem+0xd` | 4 | `old == new` | per-element field 1 |
| `elem+0x11` | 4 | `old == new` | per-element field 2 |
| `elem+0x15` | 4 | `old == new` | per-element field 3 |
| `elem+0x83` | 4 | `(old ^ new) & 4 == 0` | per-element flag bit 2 |
| `elem+0x87` | 2 | `old == new` | per-element field 4 |
| `elem+0xb1` | 4 | `old == new` | per-element field 5 |

Returns `1` (UNCHANGED) only if ALL pass; `0` (CHANGED) on first mismatch. Per-element loop runs `*(rec+0x29)` iterations with stride `0xb5`.

### 9.7 SafeArray entry format (`FUN_004d88c0`)

`FUN_004d88c0` (`0x004d88c0`) parses the SafeArray from `IProgram::Callers`. Variable-length entries. Parsing starts at byte offset 4, advances by `8 + nameLength + 0x16` (`0x1e + nameLength`) per entry:

| Entry offset | Size | Field |
|--------------|------|-------|
| `+0x00` | 2 | type/category-like field |
| `+0x02` | 4 | ID-like field |
| `+0x06` | 2 | string length (nameLength) |
| `+0x08` | nameLength | name string bytes |
| `+0x08+nameLength` | 0x16 | metadata block |

8-byte header → nameLength name bytes → fixed `0x16` (22-byte) metadata block. Each entry inserted into BST via `FUN_004d7f40` (`0x004d7f40`).

Metadata sub-fields (relative to `0x16` block start):

| Meta offset | Size | Sub-field |
|-------------|------|-----------|
| `+0x00` | 1 | metadata[0] |
| `+0x01` | 4 | metadata[1] |
| `+0x05` | 4 | metadata[2] |
| `+0x09` | 4 | metadata[3] |
| `+0x0d` | 1 | metadata[4] |
| `+0x0e` | 1 | metadata[5] |
| `+0x0f` | 2 | metadata[6] |
| `+0x11` | 1 | metadata[7] |
| `+0x12` | 2 | metadata[8] |
| `+0x14` | 2 | metadata[9] |

**BST keying distinction:** TWO distinct BSTs:
1. **Cached lookup BST** (`FUN_004e79d0`): keyed by **ProgId**. Node key at `+0x10`, value (module record ptr) at `+0x14`. Used by cached path in `FUN_004d7140`/`FUN_004d85c0`.
2. **SafeArray-built `CELPrograms` BST** (`FUN_004d7f40`): keyed by **2-byte type/category field** (node `+0x0E`) and, on equal first fields, by **name string** (node `~+0x10`). Ordered by (type, name), NOT by ProgId.

The reverse-dependent enumeration BST built by `FUN_004cad90`/`FUN_004d88c0` is the second kind. The iteration order observed by `FUN_004D91E0`'s loop (via `FUN_004d85c0` → `FUN_004d7c40` in-order walk) depends on which BST the accessor uses; `FUN_004d85c0` resolves records via the cached ProgId-keyed BST (`FUN_004e7840`) or the worker path (`FUN_004d7190`), so **iteration order is ascending ProgId** (§1.7), but **SafeArray-built BST insertion** is ordered by (type, name). These are separate data structures.

### 9.8 Unresolved gaps

1. **`IProgram` vtable offset → method-name mapping:** Semantic evidence that cmd `0x3B` = `IProgram::Callers` is conclusive, but the precise vtable index was not confirmed by locating the `CProgram` vtable in `WHServer.exe`. Type-library string order may not match vtable order. Does not affect M.4b (CLI drives cmd `0x3B` through the warehouse worker protocol, not a direct vtable call).

2. **No dynamic verification:** All M.4a conclusions are static Ghidra analysis. A Frida trace of a live TSDev verify would confirm the SafeArray at `+0x26` contains exactly the reverse-dependent set. Per AGENTS.md, UI-click automation is prohibited; a dynamic trace requires an elevated live TSDev process with a verify triggered by non-automated means.

3. **`IProgram::Callers` internal structure:** The COM-side computation that produces the Callers SafeArray (whether `WHServer.exe` queries a dependency database, traverses structured storage, or calls `elbardb.dll`) was not traced into `WHServer.exe`'s method implementation. `WHServer.exe` uses structured storage (`StgOpenStorage`/`StgCreateStorageEx`) and work-area files (`PWRKArea.wrk`, `SWRKArea.wrk`, `CWRKArea.wrk`, `DWRKArea.wrk`, `MBIFuncs.wrk`, `MCATArea.wrk`, `RWRKArea.wrk` — strings at `WHServer.exe:0x00431f98`–`0x00432040`). Outside M.4a scope.

4. **cmd `0x3B` result field:** Worker thread proc stores the COM method's return value at a warehouse offset Ghidra rendered via confusing struct-field naming. TSDev-side `FUN_004cad90` reads the SafeArray from `*(warehouse + 0x26)`, confirming the result lands at `warehouse+0x26`. Does not affect M.4b.

5. **DYNAMIC FINDING (M.4b.1, 2026-08-28) — COM server returns zero callers:**
   The M.4b.1 implementation drives cmd `0x0D` then cmd `0x3B` through the
   generic dispatcher (`FUN_101cf5b0`). Dynamic testing against a live
   TradeStation 9.5 installation confirmed:
   - cmd `0x0D` with `(category=4, ProgId=<Function>)` sets `warehouse+0x5E`
     to a non-NULL IProgram COM object. ✓
   - cmd `0x3B` succeeds with `warehouse+0x4E == 0` (status OK). ✓
   - cmd `0x3B` populates `warehouse+0x26` with a NEW SafeArray (distinct
     pointer after clearing `+0x26` to NULL before the command). ✓
   - The SafeArray descriptor is: `cDims=1, fFeatures=0x0080
     (FADF_HAVEVARTYPE), cbElements=1, cElements=4, lLbound=0`. VARTYPE
     (at `psa-4`) is `0x0011` (VT_UI1). The data is **4 bytes of zeros**. ✗
     (does not match §9.7 documented variable-length entry format)
   - Per §9.7, parsing starts at byte offset 4. A 4-byte SafeArray means
     ZERO entries. The COM server reports zero callers.

   This occurs even when both items are created/verified, the Indicator's
   source explicitly references the Function, `enumerateRecords` confirms
   both exist, the root Function is re-verified immediately before the
   query, and `refreshCache()` (cmd `0x0F` + cmd `0x4E`) is called.

   The cmd `0x0D` worker handler (case `0x0d` in `FUN_101d2150`) calls
   `IWHouse::vtable[+0x20]` (ProgramById) on the IWHouse interface at
   `warehouse+0xAE`, then `FUN_101cf020()` to obtain the IProgram COM object
   stored at `warehouse+0x5E`. The cmd `0x3B` handler calls
   `IProgram::vtable[+0x38]` (Callers) on the object at `warehouse+0x5E`.
   The protocol implementation is correct.

   **IMPORTANT CORRECTION (2026-08-28 PM directive):** The original
   characterization of this as a "stale dependency database" was WRONG.
   TSDev.exe users routinely create new items, verify them, and verify
   dependents — all without restarting TSDev. If the dependency database
   were truly stale after init, TSDev would have the same problem. It
   doesn't. Therefore the database IS updated during a session by some
   mechanism that our CLI is not performing. The zero-callers result is
   NOT a "discovery" — it is a RED test correctly failing because our
   implementation is incomplete. We are missing a step that TSDev performs.

   **WHServer.exe is an OUT-OF-PROCESS COM server** (LocalServer32
   registration, CLSID {7CEEA90C-D59E-11D3-8394-00C04F7BDF10} →
   `"C:\Program Files (x86)\TradeStation 9.5\Program\whserver.exe"`).
   elbardb.dll is loaded by WHServer.exe as the in-process implementation.
   The dependency database lives in WHServer.exe's process, NOT in ours.
   WHServer.exe was NOT running when checked, so our CLI starts a fresh
   instance with an empty dependency DB. The DB is empty for ALL items
   including existing ones with known linker callers (ProgId=517 with 28
   linker callers → 0 COM callers).

   **Session 2 finding (empirical test):** TSDev.exe was started and fully
   initialized (25-second wait); WHServer.exe spawned as its child COM
   server (confirmed running). Then `test_callers_enumeration.exe` and
   `tsdev_cli.exe dependents` were run. Result: COM Callers STILL returns 0
   for ALL items. The DB is NOT populated eagerly at startup — it must be
   populated by a specific UI action or COM command dispatched in response
   to a user action.

   **Session 3 finding — 13 missing commands:** Frida trace of TSDev.exe
   verify (driven by reusable pywinauto UI automation, `uv run tsdev-ui`)
   reveals 13 warehouse commands TSDev sends that our CLI never sends:

   `0x32 / 0x33 / 0x34 / 0x35 / 0x38 / 0x3A / 0x4D / 0x4E / 0x4F / 0x53 / 0x57 / 0x58 / 0x65`

   These commands call vtable methods on `param_1[7].Release` (same
   interface as cmd 0x3B, which our CLI does dispatch; cmd 0x0D uses
   `param_1[0xe]`). They are the leading candidates for populating the
   dependency database. **Next step:** Ghidra-analyze these 13 commands'
   vtable methods to identify which one(s) populate the dependency DB, then
   replicate them in our CLI.

   Investigation candidate statuses:
   - (a) IWHouse vtable[+0xA8] (cmd 0x0F target) — **DISPROVED** (session 3):
     `warehouse+0xAE` is non-zero and cmd 0x0F IS dispatched during CLI
     verify. Empty DB is NOT caused by cmd 0x0F being skipped.
   - (b) WHServer.exe startup scan — **DISPROVED** (session 2): TSDev.exe
     fully initialized with WHServer.exe running, DB still empty.
   - (c) elbardb.dll CWHouse initialization — **still open.**
   - (d) Frida trace of TSDev.exe verify — **COMPLETED** (session 3): 13
     missing commands found (listed above).
   - (e) `0x40A` notification handler — lower priority (DB empty for ALL items).
   - (f) Post-verify warehouse command — lower priority (DB empty for ALL items).

   See `docs/M4B1_HANDOFF.md` §4.6/§4.7/§10 and `tmp/M4B1_FINDINGS.md` F5-F8
   for full session 3 findings.

### 9.9 M.4b implementation recommendation (summary)

Based on the M.4a findings:

1. **Replace** the linker-based `enumerateReverseDependents()` (§8.7, which
   parses `record+0x4f`/`+0x29` element arrays, `element+0x7b`/`+0x73`
   dep-lists, and `0xFA3`/`0xFA4` entries) with a warehouse-level approach
   that drives cmd `0x0D` then cmd `0x3B` through the `Ordinal_10` warehouse
   singleton.
2. **Read** the reverse-dependent SafeArray from `*(warehouse + 0x26)`
   after cmd `0x3B` completes.
3. **Parse** SafeArray entries per the `FUN_004d88c0` format (§9.7).
4. **Apply** the contract-change gate (`FUN_004d9040` field offsets, §9.6)
   and recurse per `FUN_004D91E0`'s logic (§1.6–1.7, §3).
5. **Eliminate** the `0xFA3`/`0xFA4` type-code workaround and the
   expanded-whType-vs-category limitation (§8.8).

This approach operates at TSDev's own abstraction level (warehouse COM
commands), satisfies the AGENTS.md highest-level-abstraction directive, and
removes coupling to orlib20 internal linker layouts.

