# RE_M4 — Reverse-Dependent Revalidation (`FUN_004D91E0`)

> **Status:** Research-only findings note for Workstream M, slice M.4.
> **Date:** 2026-08-26
>
> **CORRECTION NOTICE (2026-08-25, PM-applied):** The original note inverted the
> contract-change gate logic in §1.6, §1.7, §2, §3.3, §3.4, and the R-M.4.2/R-M.4.3
> Answers. `FUN_004d9040` returns 1 = no change, 0 = changed (correct in the
> decompilation). `local_24 = (uint)(cVar3 == '\0')` = 1 when changed, 0 when no
> change. The gate `if ((iVar7 == 0) || (local_24 == 1))` = recurse if (NOT
> verified OR **changed**); skip if (verified AND **NOT changed**). The original
> text said the opposite ("local_24 = 1 means no change", "skip if verified AND
> changed"). All instances have been corrected below. The decompilation/
> disassembly was always correct; only the interpretive comments were wrong.
> Additionally, §6 step 3 was corrected: the contract-change check compares the
> ROOT's old vs new record (done once), not each dependent's record.
> **Scope:** Reverse-engineering of `TSDev.exe!FUN_004D91E0`, the reverse-dependent
> reverify wrapper that sits between `FUN_004D9480` and
> `orlib20!_VerifyModule_New@20` in the UI Verify command chain. No production or
> test code was changed for this task.
> **Method:** Ghidra static analysis of `TSDev.exe` and `orlib20.dll` in the
> `TSDevAnalysis` project. PE export-table parsing via `pefile` (Python) to map
> ordinal-only exports to RVAs. No TSDev reverse-traversal dynamic trace was
> captured for the original slice; §8 adds runtime diagnostic evidence for the
> linker/object-code behavior. Where static evidence is insufficient, the gap
> is recorded.
> **Binaries analyzed (read-only):**
> `C:\Program Files (x86)\TradeStation 9.5\Program\TSDev.exe`,
> `...\orlib20.dll` (baseline `9.50.01.3344`, SHA-256
> `318DB24A4F41BDB9FF69E2E647026B788706904CF887CBB0A953E003DB22EC18`).
>
> **CORRECTION NOTICE (2026-08-26):** The earlier pre-linker analysis in §§5.3–5.4
> (including its Option B2 reverse-by-inverting-forward recommendation), §6, and
> the R-M.4.5 answer is historical and superseded for production by the
> linker-based approach in §8.7. The current implementation calls the linker
> (ordinal 85), reads linked
> per-element dep-lists, and does not use `GetDependencies` or
> `_GetStaticDependencyList@12` for reverse-dependent enumeration. The §8
> diagnostic figures are unverified historical/transient evidence, not M.4
> acceptance evidence. CLI-created object code currently has version=0, which
> leaves linker element arrays empty and blocks the M.4 dependency test.

---

## 0. Executive Summary

`FUN_004D91E0` is the **TSDev.exe-side** reverse-dependent reverify wrapper. It
is **not** present in orlib20.dll; it is private to TSDev.exe. Its behavior:

1. **Verifies the current module** by calling `orlib20!_VerifyModule_New@20`
   (ordinal 700) — the same primitive the standalone CLI already uses.
2. **Only on success**, and only for certain categories (0, 1, 4), it
   **enumerates reverse dependents** of the just-verified module via a
   TSDev-internal `CELPrograms` BST that is populated from the orlib20
   warehouse (worker-thread command `0x0D` = `IWHouse::ProgramById`, or a
   cached BST lookup via `Ordinal_71`/`InitGlobalLookup`).
3. For each reverse dependent, it performs a **metadata-based contract-change
   comparison** (`FUN_004d9040`) between the old and new module records. This
   comparison is **purely warehouse-record metadata** — it does NOT parse
   EasyLanguage source text. (R-M.4.2 — no AGENTS.md violation.)
4. It **recurses** into itself (`FUN_004d91e0` at `0x004D9402`) for each
   reverse dependent, subject to a per-dependent gate. There is **no
   visited-set and no depth counter** — cycle protection relies entirely on the
   contract-change check and a per-record "already-verified" flag
   (`FUN_004ca5c0`, `*(byte*)(record+0x55) & 1`). (R-M.4.3.)
5. Per-dependent verify failures are **accumulated, not aborting**: the
   compiler object's `+0x04` field is `AND`-ed with each verify result
   (`param_1[1] = param_1[1] & uVar6`), so any failure drives the aggregate to
   0, but the loop continues. The root result (`+0x00`) is only set from the
   root verify when the primary-flag is 1. (R-M.4.4.)

**Critical feasibility finding (R-M.4.5):** orlib20.dll does **not** export a
single "reverse-dependent revalidation" API. The reverse-dependent enumeration
is performed by TSDev.exe-private code (`FUN_004d7140`, `FUN_004c9670`,
`FUN_004d85c0`) that drives the orlib20 warehouse via:
- `Ordinal_10` (`GetWarehouse`, RVA `0x1A9650`) — warehouse singleton,
- `Ordinal_71` (`InitGlobalLookup`, RVA `0x1A9460`) — global lookup init,
- `Ordinal_28` (RVA `0x1A03F0`) — master-record lookup by type/name,
- `Ordinal_69` (RVA `0x1A3E00`) — module-record retrieval,
- `Ordinal_68` (RVA `0x1A87D0`) — count accessor (`*(obj+0x0C)`),
- `Ordinal_70` (RVA `0x15DF30`) — refcount-decrement/free,
- `_VerifyModule_New@20` (ordinal 700, RVA `0x169630`) — the verify primitive
  the CLI already uses.

orlib20 does export **forward** dependency-list APIs (`GetDependencies`
ordinal 487, `_GetDynamicDependencyList@12` ordinal 489,
`_GetStaticDependencyList@12` ordinal 531, `_DeleteDependencyList@4` ordinal
397), but these return what a module depends on, not what depends on a module.
The reverse-dependent set TSDev traverses is built in TSDev.exe from the
warehouse master records, not from an orlib20 reverse-dependency export.

**Historical recommendation (superseded by §8.7):** The original note proposed
composing `GetWarehouse`/`Ordinal_10`, master-record enumeration, and
`_VerifyModule_New@20` with a TSDev-equivalent metadata comparison. The current
CLI implementation instead composes `_LoadMasterRecsEX@16`, Ordinal_28, the
linker (ordinal 85), Ordinal_70, and `_VerifyModule_New@20`, scanning linked
per-element dep-lists. It does not use `GetDependencies` for this path.

---

## 1. R-M.4.1 — Exact Traversal

### Question

Confirm the exact traversal `FUN_004D91E0` performs. Which warehouse records
enumerate reverse dependents? In what order? What per-dependent work is done
(reverify only? recompile? both?)? Trace the function and its callees.

### Evidence

#### 1.1 Function signature and entry

`FUN_004D91E0` at `TSDev.exe:0x004D91E0` is `__thiscall` (`ECX` = compiler
object). Disassembly entry (`0x004d9201`–`0x004d9206`):

```asm
004d9201  MOV EDI,[EBP + 0x8]    ; param_2 (stack) = category (whType)
004d9204  MOV EBX,ECX           ; this = compiler object
```

The caller `FUN_004D9480` (`0x004D9480`) sets up the call (`0x004D9488`–
`0x004D94A0`):

```asm
004D9488  PUSH dword ptr [ESI+0x10]   ; arg3 (stack) = compiler+0x10 = ProgId
004D948B  MOV  ECX,ESI               ; this = compiler object
004D948D  MOV  dword ptr [ESI],0      ; compiler+0x00 = 0 (root result)
004D9493  PUSH dword ptr [ESI+0x0C]   ; arg2 (stack) = compiler+0x0C = subtype
004D9496  MOV  dword ptr [ESI+4],0    ; compiler+0x04 = 0 (aggregate result)
004D949D  PUSH dword ptr [ESI+8]      ; arg1 (stack) = compiler+0x08 = category
004D94A0  CALL 0x004D91E0
```

Decompilation of `FUN_004D9480`:

```c
void __fastcall FUN_004d9480(undefined4 *param_1) {
    StartCompilingProcess();
    *param_1 = 0;            // compiler+0x00 = 0
    param_1[1] = 0;         // compiler+0x04 = 0
    FUN_004d91e0(param_1[2], param_1[3], param_1[4]);  // category, subtype, ProgId
    EndCompilingProcess();
}
```

So `FUN_004D91E0(this=compiler, category, subtype, ProgId)`. The compiler
object layout (confirmed by M.1, `docs/M1_HANDOFF.md` §R-M.1.2, and
`FUN_00451800` at `0x00451800`): `+0x08`=category, `+0x0C`=subtype,
`+0x10`=ProgId, `+0x18`=HWND (notification window). `+0x00` and `+0x04` are
result accumulators zeroed before the call.

#### 1.2 Step 1 — Verify the current (root) module

After normalizing the category via `FUN_004d7a30` (a category→power-type
expander; e.g., category 2 maps to subtypes `0x402`, `0x502`, …, `0x1402`),
`FUN_004D91E0` calls `_VerifyModule_New@20` (`0x004D9299`):

```asm
004d928b  PUSH EBX                      ; arg5 = compiler object (param_5)
004d928c  PUSH ECX                      ; arg4 = primary-flag (1 if root==this)
004d928d  PUSH EAX                      ; arg3 = ProgId (param_4)
004d928e  PUSH EDI                      ; arg2 = category (whType)
004d928f  PUSH dword ptr [EBX + 0x18]   ; arg1 = compiler+0x18 (HWND)
004d9299  CALL 0x0053f5bc               ; _VerifyModule_New@20 thunk
```

The primary-flag (`ECX`) is computed at `0x004d9278`–`0x004d928b`:

```asm
004d9278  CMP [EBX + 0x8], EDI    ; compiler+0x08 (saved category) == param_2?
004d927b  JNZ 0x004d9289
004d927d  CMP [EBX + 0x10], EAX   ; compiler+0x10 (saved ProgId) == param_4?
004d9280  JNZ 0x004d9289
004d9282  MOV ECX, 0x1            ; primary = 1 (this IS the root module)
004d9287  JMP 0x004d928b
004d9289  XOR ECX, ECX            ; primary = 0 (this is a reverse dependent)
```

So the primary-flag is 1 only when the current `(category, ProgId)` matches the
compiler object's saved root `(+0x08, +0x10)`. This is the "+0x10 root
bookkeeping/primary-flag selection" noted in prior RE
(`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md` §F
("Disproved or non-causal hypotheses")).

Decompiled verify + result handling:

```c
uVar6 = _VerifyModule_New_20(param_1[6], uVar2, param_4, param_2, param_1);
if (param_2 == (undefined4 *)0x1) {   // if primary-flag was 1
    *param_1 = uVar6;                  //   compiler+0x00 = root result
}
param_1[1] = param_1[1] & uVar6;       // compiler+0x04 &= verify result (aggregate)
```

`_VerifyModule_New@20` (`orlib20.dll:0x10169630`, ordinal 700) compiles a
single module via `CELBasicCompiler::CompileText` and returns 1 on success
(decompilation confirms: it calls `FUN_101a9650`/GetWarehouse,
`FUN_101cff60`/master-record lookup, `CELBasicCompiler::CompileText`, then
`FUN_101698f5` for finalization; no reverse-dependent traversal inside
orlib20). So **the root verify is a full recompile** (CompileText), not a
lightweight check.

#### 1.3 Step 2 — Gate: only verify dependents on success, only for categories 0/1/4

After the root verify returns, the reverse-dependent path is gated
(`0x004d92a9`–`0x004d92bd`):

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

So reverse dependents are processed **only when the root verify succeeded AND
the category is 0 (Strategy), 1 (Function), or 4 (SystemFunction)**. Other
categories (2 Indicator, 3 ActivityBar, etc.) skip the reverse-dependent
traversal entirely.

#### 1.4 Step 3 — Build the reverse-dependents enumerator

On the gated path, `FUN_004D91E0` calls `Ordinal_10` (GetWarehouse) at
`0x004d92c5`, then `FUN_004d7140` at `0x004d92e9`:

```c
iVar5 = Ordinal_10(0);                         // GetWarehouse(NULL) -> warehouse
if (iVar5 != 0) {
    param_2 = (undefined4 *)0x0;
    iVar5 = FUN_004d7140(param_3, param_4, &param_2);  // build enumerator
    if (iVar5 == 1) { ... }
}
```

`FUN_004d7140` (`0x004D7140`) has two paths (decompilation):

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

- **Cached path** (`*(warehouse + 0xc2) == 1`): calls `Ordinal_71`
  (`InitGlobalLookup`, orlib20 RVA `0x1A9460`) then `FUN_004e79d0`, which does
  a BST search by ProgId (`param_3`) in a per-type tree; if not found, falls
  back to `Ordinal_10` + `FUN_004d75b0`.
- **Worker path** (`FUN_004d75b0`, `0x004D75B0`): sets
  `*(warehouse + 0x12) = param_2` (type), `*(warehouse + 0x16) = param_3`
  (ProgId), `*(warehouse + 4) = 0xd` (**command 0x0D = `IWHouse::ProgramById`**),
  signals the worker thread, waits, and on success allocates a `0x241`-byte
  record via `FUN_004c71f0` and increments its `+0x210` refcount.

The output `*param_4` is a **module record pointer** for the just-verified
module — this becomes the "current record" used for the contract-change
comparison.

#### 1.5 Step 4 — Initialize the `CELPrograms` BST and get the dependent count

`FUN_004D91E0` then constructs a `CELPrograms` enumerator object
(`FUN_004d80d0` at `0x004d92fa`, which sets `*param_1 = CELPrograms::vftable`
and builds a BST via `FUN_004d8a90`), initializes it (`FUN_004c9670` at
`0x004d930a`), and gets the count via `Ordinal_68` at `0x004d931c`:

```c
FUN_004d80d0(local_40);              // construct CELPrograms
iVar5 = FUN_004c9670(local_40);      // initialize (populates BST from warehouse)
if (iVar5 == 1) {
    local_20 = Ordinal_68(local_40);  // count = *(obj + 0x0C)
    if (0 < local_20) { ... loop ... }
}
```

- `FUN_004c9670` (`0x004C9670`) calls `FUN_004cad90` which (under a
  `CSingleLock`) calls `FUN_004d7040` to get a record, then if a SafeArray is
  present at `*(record + 0x26)`, calls `SafeArrayAccessData` +
  `FUN_004d88c0` to populate the BST from the SafeArray, then
  `SafeArrayDestroy`. So the BST is populated from warehouse SafeArray data
  (binary records, not source text). `FUN_004c9670` then reads the populated
  count **internally** via `FUN_004d85b0` (`TSDev.exe:0x004D85B0`, called at
  `0x004c9699`) — a one-line TSDev-side accessor `return *(undefined4
  *)(param_1 + 0xc);` — and iterates `0..count-1` calling `FUN_004d85c0` per
  index. `FUN_004d85b0` is therefore the in-process counterpart of the
  orlib20 count accessor (see next bullet) and is part of the M.4 path.
- `Ordinal_68` (`orlib20.dll:0x101A87D0`, ordinal 68) is a thunk to
  `FUN_101e5970` which returns `*(param_1 + 0x0C)` — the count field of the
  `CELPrograms`/record object. `FUN_004D91E0` itself reads the count a second
  time via this export at `0x004d931c` (the loop bound used by the
  reverse-dependent loop in §1.7). Both `FUN_004d85b0` (inside
  `FUN_004c9670`) and `Ordinal_68` (in `FUN_004D91E0`) read the same
  `*(obj + 0x0C)` count field; this is the **reverse-dependent count**.

#### 1.6 Step 5 — Build the "new" record and compare contracts

Before the loop, `FUN_004D91E0` builds a fresh "new" record via `FUN_004d8fb0`
(`0x004d9336`) and runs the contract-change comparison `FUN_004d9040`
(`0x004d9346`):

```c
FUN_004d8fb0(param_3, uVar2);            // build "new" record (calls Ordinal_28 + Ordinal_69)
cVar3 = FUN_004d9040(&local_18);         // compare old vs new; returns 1 if SAME (no change), 0 if CHANGED
local_24 = (uint)(cVar3 == '\0');        // 1 if CHANGED, 0 if no change (CORRECTED — see notice)
```

`FUN_004d8fb0` (`0x004D8FB0`) mirrors the root setup: it calls `Ordinal_28`
(master-record lookup by type/name) and, on success, `Ordinal_69` (module
record retrieval) to produce the freshly-verified record. (See R-M.4.2 for the
comparison.)

#### 1.7 Step 6 — Per-dependent loop

The loop (`0x004d9357`–`0x004d9415`) iterates `iVar5 = 0 .. local_20-1`:

```c
for (iVar5 = 0; iVar5 < local_20; iVar5 = iVar5 + 1) {
    param_4 = 0;
    iVar7 = FUN_004d85c0(iVar5, &param_4);   // get reverse-dependent record by index
    if (iVar7 == 1) {
        local_2c = param_1[6];                // save compiler+0x18 (HWND)
        // resolve the dependent's "module record" pointer:
        if (*(char *)(param_4 + 0x8e) == '\0') {
            iVar7 = FUN_004c9ef0();           // returns *(record + 0x8f)
        } else {
            iVar7 = *(int *)(param_4 + 0x8f);
        }
        param_1[6] = *(uint *)(iVar7 + 0xe4); // compiler+0x18 = dependent's record+0xe4
        iVar7 = FUN_004ca5c0();               // per-dependent "already verified?" check
        if ((iVar7 == 0) || (local_24 == 1)) { // recurse if NOT verified OR contract CHANGED (CORRECTED)
            // re-resolve record, get type/subtype/sig, then recurse:
            ...
            FUN_004d91e0(uVar4);              // RECURSE (0x004D9402)
        }
        param_1[6] = local_2c;                // restore compiler+0x18
        FUN_00410c00();                       // refcount-dec the dependent record
    }
}
```

- `FUN_004d85c0` (`0x004D85C0`) is the per-index BST accessor: it validates
  `0 <= index < *(obj + 0x0C)` (the count), walks the BST via `FUN_004d7c40`
  (an in-order BST traversal by index), and returns the record at that index
  via `FUN_004d70a0` (cached path: `Ordinal_71` + `FUN_004e7840`) or
  `FUN_004d7190` (worker path). The record's name is at `+0x0e` (or `+0x10` if
  the string is heap-allocated, gated by `*(uint*)(record+0x24) > 0xf`).
- **Order**: the BST is traversed in **ascending in-order** (left-root-right)
  via `FUN_004d7c40`, which is a standard red-black-tree successor/predecessor
  walk. The records are keyed by ProgId. So reverse dependents are visited in
  **ascending ProgId order**. This is now confirmed by direct decompilation
  (see "Traversal-order evidence" below).

##### Traversal-order evidence (FUN_004d7c40, FUN_004e79d0, FUN_004e7840)

`FUN_004d7c40` (`TSDev.exe:0x004D7C40`) is the in-order walk. Decompilation:

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

Node layout (MSVC `std::_Tree` node): `piVar4[0]` = `_Left`, `piVar4[1]` =
`_Parent`, `piVar4[2]` = `_Right`, the byte at `+0xd` (high byte of
`piVar4[3]`) = `_Isnil`/color flag, and `piVar4[4]` (offset `0x10`) = the key.
The forward branch implements the textbook BST successor: descend the right
subtree then walk to its leftmost node; if no right subtree, walk up the
parent chain until the node is a left child. This is an in-order
(left→node→right) traversal advanced one step per call, confirming
**ascending in-order order**.

`FUN_004e79d0` (`0x004E79D0`, cached BST lookup) confirms the key is ProgId.
Decompilation (key comparison excerpt):

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

`param_3` is the ProgId (passed from `FUN_004d7140` as the second stack arg,
which is the root module's ProgId — see §1.4). The BST search compares
`puVar3[4]` (node offset `0x10`) against `param_3` (ProgId), so **the BST key
is ProgId at node offset `0x10`**. The value at node offset `0x14`
(`puVar4[5]`) is the module-record pointer returned to the caller.

`FUN_004e7840` (`0x004E7840`, the per-record cached lookup used by
`FUN_004d85c0`'s cached path) confirms the same keying:

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
            if ((int)puVar4[4] < *(int *)(param_2 + 0x1e)) {  // node KEY (puVar4[4], +0x10) vs record's ProgId (+0x1e)
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

Here the lookup key is `*(int *)(param_2 + 0x1e)` — the ProgId field of the
dependent's record (`param_2` is a record pointer, `+0x1e` is its ProgId) —
again compared against the node key at `puVar4[4]` (offset `0x10`).

**Correction to a prior draft of this note:** an earlier version cited the
key as `puVar4[4]` / `*(int*)(node+0x14)`. The `+0x14` citation was wrong:
`+0x14` (`puVar4[5]`) is the **value** (module-record pointer), not the key.
The key is `puVar4[4]` at node offset **`0x10`**, compared with the ProgId.
The order conclusion (ascending ProgId, BST in-order) is unaffected and is
now confirmed by direct decompilation of all three functions.
- **Per-dependent work**: the recursive call `FUN_004d91e0(uVar4)` re-enters
  the whole function for the dependent, which calls `_VerifyModule_New@20` →
  `CompileText`. So each reverse dependent is **recompiled+verified** (full
  `CompileText`), not merely checked.

#### 1.8 Step 7 — Cleanup

After the loop, the enumerator record is released via `Ordinal_70`
(`0x004d941b` region: `if (local_1c != 0) Ordinal_70(local_1c, 0)`), the
`CELPrograms` object is destroyed (`FUN_004d81f0` at `0x004d942a`-area), and
the function returns through `FUN_004d945f` which calls `Ordinal_70` on the
remaining record. `Ordinal_70` (`orlib20.dll:0x1015DF30`, ordinal 70) is a
refcount-decrement-and-free: it decrements `*(param_1 + 0xca)`, and on
reaching 1 frees `+0xd2` and the object itself.

### Answer (R-M.4.1)

- **Traversal**: `FUN_004D91E0` first verifies the root module via
  `_VerifyModule_New@20` (full `CompileText` recompile). On success, for
  categories 0/1/4 only, it builds a `CELPrograms` BST of reverse dependents
  from the orlib20 warehouse (command `0x0D` `ProgramById` or cached
  `InitGlobalLookup` BST), then iterates them in **ascending ProgId (BST
  in-order) order**.
- **Per-dependent work**: each reverse dependent is **recompiled+verified**
  via a recursive `FUN_004d91e0` call, which invokes
  `_VerifyModule_New@20`/`CompileText`. It is not a lightweight check.
- **Records**: the reverse dependents are enumerated from a TSDev-internal
  `CELPrograms` BST populated from warehouse SafeArray data (binary master
  records), keyed by ProgId (node key at offset `0x10`, confirmed by
  decompilation of `FUN_004e79d0`/`FUN_004e7840` — see §1.7 "Traversal-order
  evidence"). The orlib20 warehouse provides the data via
  `GetWarehouse`/`Ordinal_10`, `InitGlobalLookup`/`Ordinal_71`, and the
  worker-thread command `0x0D`.

---

## 2. R-M.4.2 — Contract-Change Detection

### Question

Confirm how TSDev detects an interface/contract change (signature comparison,
version field, GUID, parameter list, etc.). CRITICAL: the detection must be
warehouse/metadata-derived. If it parses EasyLanguage SOURCE TEXT, flag it.

### Evidence

The contract-change check is `FUN_004d9040` at `TSDev.exe:0x004D9040`, called
at `0x004d9346` with `&local_18` (where `local_18` was set by `FUN_004d9040`'s
caller from the old and new record pointers). Full decompilation:

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

**Return convention**: returns **1 if NO contract change** (records are
contract-equivalent), **0 if changed** (or null/incompatible). The caller
inverts: `local_24 = (uint)(cVar3 == '\0')` → `local_24 = 1` means **changed**
(CORRECTED — the original text said "no change", which was wrong: `cVar3 == 0`
is true when the function returned 0 = changed).

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

Every compared field is a **fixed-offset numeric/bitmask field inside the
warehouse record structure** (the `0x10A`-byte master record format, or the
module record derived from it). There are **no string reads, no source-text
pointers, no EasyLanguage parsing** in `FUN_004d9040`. The array at `+0x4f`
with stride `0xb5` is a parameter/contract-element table stored as binary
metadata in the record.

The records being compared are obtained from the warehouse via `Ordinal_28`
and `Ordinal_69` (both metadata-record lookups), not from source files. The
"new" record is built by `FUN_004d8fb0` which calls `Ordinal_28` (master
record) + `Ordinal_69` (module record) — pure warehouse metadata.

### Answer (R-M.4.2)

- **Mechanism**: `FUN_004d9040` performs a **field-by-field metadata
  comparison** of two warehouse module records (old vs new), checking type
  pointer (`+0xd6`), several bitmask fields (`+0x15`, `+0x7f`, `+0x81`), an
  element count (`+0x29`), and a per-element comparison of a contract array
  (`+0x4f`, stride `0xb5`, comparing `+0xd`/`+0x11`/`+0x15`/`+0x83`/`+0x87`/
  `+0xb1` per element).
- **No source-text parsing**: confirmed. Every compared field is a
  fixed-offset numeric/bitmask field in the warehouse record. No string
  reads, no source-text pointers, no EasyLanguage tokenization. This is
  **fully compliant with AGENTS.md** — metadata comes from orlib20 warehouse
  records, not item source content.
- **Return**: 1 = contracts equivalent (no change), 0 = changed. The caller
  inverts this into `local_24` (1 = changed, 0 = no change) and uses it to gate
  recursion: recursion is skipped only when the dependent is already verified
  AND the contract did NOT change (see R-M.4.3). If the contract changed,
  recursion always proceeds (CORRECTED — original text inverted the gate).

---

## 3. R-M.4.3 — Recursion / Cycle Handling

### Question

Confirm recursion/cycle handling. Does `FUN_004D91E0` guard against cycles
(A depends on B depends on A)? What is the visited-set mechanism? Max depth?

### Evidence

#### 3.1 The recursive call

The recursive call is at `0x004D9402`:

```asm
004d93ff  MOV ECX, EBX            ; this = compiler object (same as parent)
004d9401  PUSH EAX                ; arg = GetSigFromType(record, subtype, ...)
004d9402  CALL 0x004d91e0          ; RECURSE
```

The recursive call passes the **same compiler object** (`EBX`) as `this`, with
the dependent's category/subtype/ProgId derived from the dependent's record.

#### 3.2 No visited-set, no depth counter

Exhaustive review of the disassembly and decompilation of `FUN_004D91E0`
reveals:
- **No visited-set data structure** is allocated or checked. There is no
  `std::set`, no hash table, no per-call bitmap tracking visited ProgIds.
- **No depth counter** is passed or incremented. The function's stack frame
  (`SUB ESP, 0x13c` = 316 bytes) contains only locals for the current
  iteration; no depth/visited field is threaded through the recursive call.
- The only state threaded through recursion is the **compiler object** itself
  (`EBX`/`param_1`), whose `+0x18` field is saved/restored around each
  dependent (`local_2c = param_1[6]` ... `param_1[6] = local_2c`).

#### 3.3 The cycle-protection mechanism: per-record "verified" flag + contract gate

The recursion is gated by two checks at `0x004d93a8`–`0x004d93b5`:

```c
iVar7 = FUN_004ca5c0();              // check dependent's "verified" flag
if ((iVar7 == 0) || (local_24 == 1)) {   // recurse if NOT verified OR contract CHANGED (CORRECTED)
    // ... recurse ...
}
```

- `FUN_004ca5c0` (`0x004CA5C0`) returns `*(byte *)(param_1 + 0x55) & 1` after
  optionally calling `FUN_004cb430` (a lazy-init). So it reads **bit 0 of the
  dependent record's `+0x55` field** — a per-record "already verified" flag.
- `local_24` is the contract-change result inverted: `local_24 == 1` means
  **contract CHANGED** (CORRECTED — original text said "no contract change",
  which was wrong; `local_24 = (cVar3 == 0)` is 1 when `FUN_004d9040` returned
  0 = changed).

**Gate logic** (decompiled, `0x004d93ad`–`0x004d93b5`):

```asm
004d93a8  CALL 0x004ca5c0       ; iVar7 = verified-flag
004d93ad  TEST EAX, EAX
004d93af  JZ   0x004d93b7       ; if iVar7 == 0 (NOT verified) -> recurse
004d93b1  CMP  [EBP - 0x20], 1  ; local_24 == 1? (contract CHANGED — CORRECTED)
004d93b5  JNZ  0x004d9407       ; if verified AND contract NOT changed (local_24 != 1) -> skip recurse
```

So recursion happens when:
- `iVar7 == 0` (dependent NOT yet verified), OR
- `local_24 == 1` (contract CHANGED — re-verify needed because the root's
  signature/contract is different, CORRECTED).

Recursion is **skipped** only when the dependent is **already verified AND**
the contract **did NOT change**. The rationale: if the root's contract is
unchanged and the dependent was already verified against that same contract,
there is nothing to revalidate; TSDev skips it. If the contract DID change,
the dependent must be revalidated even if already verified (CORRECTED —
original text said the opposite).

#### 3.4 How cycles are actually bounded

There is **no explicit cycle guard**. The protection is implicit:
1. The `+0x55` "verified" flag is set during `_VerifyModule_New@20`/compile
   (a successfully verified module is marked). On a second visit, if the
   contract did NOT change, the gate `(verified && NOT changed)` skips
   recursion (CORRECTED — original text said the gate skips on "changed").
2. If a true cycle existed (A→B→A) with no contract change, the recursion
   would be skipped on the second visit because the dependent is already
   verified AND the contract didn't change. The bound is the
   **contract-change check**: once a module is verified, its record's
   contract fields don't change, so `FUN_004d9040` returns "no change"
   (1), `local_24 = 0` (not changed), and the gate `(verified AND NOT
   changed)` skips. If the contract DID change, the recursion proceeds
   regardless of verified status (CORRECTED).

**Static analysis cannot fully rule out unbounded recursion in a pathological
cycle.** The `+0x55` flag and contract gate reduce the cascade but are not a
hard visited-set. A dedicated dynamic trace with a synthetic cycle would be
needed to confirm whether the warehouse's reverse-dependent set can ever
contain a cycle (the warehouse likely prevents cycles at insert time, but
that is not proven here).

#### 3.5 No max depth

No depth limit is enforced. Stack overflow is bounded only by the 316-byte
frame per level and the OS stack (the TSDev verify thread is not documented
to have an enlarged stack for this path).

### Answer (R-M.4.3)

- **Visited-set**: **none.** There is no visited-set, no depth counter, no
  per-call bitmap. The only cycle mitigation is a **per-record "verified"
  flag** at `*(byte*)(record + 0x55) & 1`, read by `FUN_004ca5c0`.
- **Gate**: recursion is skipped only when the dependent is already verified
  (`+0x55` bit 0 set) **AND** the contract did NOT change (`FUN_004d9040`
  returned "no change" = 1, `local_24 = 0`). If the contract changed
  (`local_24 = 1`), recursion always proceeds regardless of verified status
  (CORRECTED — original text inverted the gate).
- **Max depth**: **none.** No depth limit is enforced.
- **Cycle safety**: not guaranteed by an explicit guard. The protection is
  implicit (verified flag + contract-change gate). Whether the warehouse
  prevents cycles in the reverse-dependent set is **not proven statically**
  — see §7 ("Gaps / Open Questions").

---

## 4. R-M.4.4 — Failure Reporting

### Question

When a reverse-dependent reverify fails, does TSDev fail the whole operation,
collect per-dependent errors, or warn? How are dependent failures surfaced?

### Evidence

#### 4.1 Aggregate result accumulation

The verify result handling (`0x004d929e`–`0x004d92a6`):

```c
uVar6 = _VerifyModule_New_20(param_1[6], uVar2, param_4, param_2, param_1);
if (param_2 == (undefined4 *)0x1) {   // primary-flag was 1 (root)
    *param_1 = uVar6;                  // compiler+0x00 = root result
}
param_1[1] = param_1[1] & uVar6;       // compiler+0x04 &= result (aggregate AND)
```

This applies to **every** verify call — root and recursive. The compiler
object's `+0x04` is a running **logical AND** of all verify results. Any
single failure (return 0) drives `+0x04` to 0 permanently. The `+0x00` field
holds the **root** result (only set when primary-flag == 1).

#### 4.2 The loop does NOT abort on failure

The per-dependent loop (`0x004d9357`–`0x004d9415`) has no early-exit on
verify failure. The recursive `FUN_004d91e0` call returns, the loop increments
`EDI` (`0x004d9415: INC EDI`) and continues to the next dependent regardless
of the returned result. There is no `break` on failure.

#### 4.3 How the result is consumed

The caller `FUN_004D9480` does not inspect the result directly — it calls
`EndCompilingProcess()` and returns. The result is read in `FUN_00451800`
(the UI Verify handler) after `FUN_004d9480` returns. Decompilation of
`FUN_00451800` (`TSDev.exe:0x00451800`), compiler-object setup and branch:

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

**(a) `piVar10` mapping — confirmed.** `piVar10 = (int *)Ordinal_66()`
(`Ordinal_66` is the CELCompiler alloc+ctor, orlib20 RVA `0x165F70`; see
§5.2) returns a pointer to the **start** of the freshly allocated compiler
object. The subsequent field writes index from that
base: `piVar10[2]`→`+0x08` (category), `piVar10[3]`→`+0x0C` (subtype),
`piVar10[4]`→`+0x10` (ProgId), `piVar10[6]`→`+0x18` (HWND) — exactly the
layout established in §1.1. The branch condition `if (*piVar10 == 1)`
therefore reads `piVar10[0]` = **`compiler+0x00` (the root result)**, not
`+0x04`. The Ghidra-generated name `piVar10` is anchored to `+0x00` by the
`Ordinal_66()` assignment, so the snippet proves which field is checked.

**(b) `+0x04` not checked downstream — confirmed for `FUN_00451800` and its
immediate callees.** After the `if (*piVar10 == 1)` branch, the compiler
pointer (`piVar10` / saved `local_14`) is consumed only by:

- the success path: a virtual call `(**(code **)(*(int *)param_1 + 0x1f8))()`
  on the **CDocument** (`param_1`), which does **not** receive the compiler
  pointer and therefore cannot read `+0x04`;
- the failure path: `FUN_0044a1e0()` (constructs a `CELProgramWriter`
  object — sets `CELProgramWriter::vftable` and zeroes `+0x241..+0x26d`; it
  does not receive or read the compiler object);
- `FUN_0044c410()` (called with `piStack_2c0 = piVar10`); decompilation
  shows it calls `_GetCompilingErrors_4()` and iterates the compiler's
  **error list** (record fields `+0x0e`, `+0x20`, `+0x34`, `+0x38`, `+0x50`,
  `+0x68`, `+0x80`, …) — it does **not** read `piVar10[1]` (`+0x04`);
- `FUN_0044b880()` (no args; just `AfxGetThread` message-pump calls);
- `FUN_004502f0()` (receives the **CDocument** `param_1`, not the compiler;
  reads CDocument fields `+0x7a`, `+0x1ea`, `+0x75`, …);
- `Ordinal_67()` (CELCompiler dtor+free — frees the object, does not branch
  on `+0x04`).

None of these read `compiler+0x04`. So within `FUN_00451800` and its
immediate callees after the branch, **the aggregate `+0x04` is not
consulted**; the UI success/failure decision is driven solely by the root
result `+0x00`. The `+0x04` aggregate is computed by `FUN_004D91E0`
(`param_1[1] = param_1[1] & uVar6` per dependent) but is dead with respect
to this UI path. (Whether `+0x04` is consumed by any deeper downstream
UI/reporting path beyond the immediate callees listed above was not
exhaustively traced — see §7 Gaps.)

Because `*param_1 = uVar6` is executed only when `param_2 == 1` (primary),
and the recursive calls pass `param_2 == 0` (the dependent's
`(category, ProgId)` does not match the compiler's root `(+0x08, +0x10)`),
`+0x00` is written only by the root verify. A dependent failure does **not**
overwrite `+0x00`. Combined with (b), this means **the UI reports
success/failure based on the root verify result alone**; dependent failures
are collected as compiler errors (via the standard `CompileText` error
machinery) but do not flip the root success flag and do not affect the
`FUN_00451800` branch.

#### 4.4 Compiler error collection

`_VerifyModule_New@20` → `CompileText` populates the compiler's error list
(the `StartCompilingProcess`/`EndCompilingProcess` pair and the
`FUN_101559b0`/error-collection machinery in orlib20). Errors from each
compile are collected in the compiler object's error structures. On the
failure branch, `FUN_00451800` calls `FUN_0044c410`, which invokes
`_GetCompilingErrors_4()` and iterates the error list to surface per-record
errors (file-type/name/path/line fields per error entry). So per-dependent
compile errors are collected in the compiler's error list and surfaced
through `FUN_0044c410`, but the **success/failure decision is the root
result**. (`FUN_0044a1e0`, also called on the failure branch, is a
`CELProgramWriter` constructor — it sets `CELProgramWriter::vftable` and
zeroes `+0x241..+0x26d`; it is not the error-display routine.)

### Answer (R-M.4.4)

- **No abort**: the per-dependent loop does NOT abort on a dependent verify
  failure. It continues to the next dependent.
- **Aggregate accumulation**: the compiler's `+0x04` field is a running AND
  of all verify results (root + every dependent). Any failure drives `+0x04`
  to 0.
- **Root result drives UI (confirmed with evidence, §4.3)**: the UI
  success/failure branch in `FUN_00451800` checks `compiler+0x00` (the
  **root** result), established by the assignment `piVar10 = (int
  *)Ordinal_66()` (compiler base) and the branch `if (*piVar10 == 1)`.
  `+0x00` is only set by the root verify (primary-flag == 1); recursive
  dependent calls pass primary-flag 0 and do not write `+0x00`. A dependent
  failure does NOT overwrite `+0x00`. So **the UI reports success/failure
  based on the root verify alone**; dependent failures are collected as
  compiler errors but do not flip the root success flag.
- **`+0x04` not consumed by the immediate UI path (confirmed with evidence,
  §4.3)**: decompilation of `FUN_00451800` and its immediate callees
  (`FUN_0044a1e0`, `FUN_0044c410`, `FUN_0044b880`, `FUN_004502f0`,
  `Ordinal_67`) shows none read `compiler+0x04` after the branch. The
  aggregate is computed by `FUN_004D91E0` but is dead with respect to this UI
  path. Whether `+0x04` is consumed by any deeper downstream UI/reporting
  path beyond these immediate callees was not exhaustively traced (see §7
  Gaps).
- **Error surfacing**: per-dependent compile errors are collected in the
  compiler's error list (via the standard `CompileText` error machinery) and
  displayed through `FUN_0044c410` on the failure path. There is no separate
  per-dependent error report structure in `FUN_004D91E0` itself.
- **No warning-only path**: there is no "warn but succeed" branch. The
  function either verifies (1) or not (0); errors are collected but the
  decision is binary on the root result.

---

## 5. R-M.4.5 — Map to orlib20 / Warehouse Primitives for the Standalone CLI

### Question

Map the TSDev.exe wrapper's behavior onto supported orlib20/warehouse
primitives available to the standalone CLI. Do NOT call TSDev.exe's private
`FUN_004D91E0`. (a) Does orlib20 export a reverse-dependent revalidation API?
(b) If not, what primitives compose the traversal? (c) Compare direct API
vs. minimal replication and recommend.

### Evidence

#### 5.1 orlib20 export table — reverse-dependent / dependency APIs

PE export-table scan (`pefile` on
`C:\Program Files (x86)\TradeStation 9.5\Program\orlib20.dll`, ordinal base
10) for dependency-related exports:

| Ordinal | Name | RVA | Direction |
|---:|---|---:|---|
| 487 | `GetDependencies` | `0x153F00` | **forward** (what module depends on) |
| 489 | `_GetDynamicDependencyList@12` | `0x1A9060` | **forward** (dynamic dep list) |
| 531 | `_GetStaticDependencyList@12` | `0x1A8F70` | **forward** (static dep list) |
| 396/397 | `_DeleteChangedModulesList@4` / `_DeleteDependencyList@4` | `0x1A9150` | free a dep list |

Decompilation of `GetDependencies` (`0x10153F00`):

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

`GetDependencies` returns the **forward** dependency list of a module (what it
depends on), gated on `local_d3 & 4` (a "has dependencies" flag in the
record). `_GetDynamicDependencyList@12` and `_GetStaticDependencyList@12`
both walk `DAT_1023dccc` (a global tree) and build arrays of
`{param_2, param_3, ...}` entries — these are **forward** dependencies (the
module's own deps), not reverse dependents (callers).

**No orlib20 export performs reverse-dependent enumeration or
reverse-dependent revalidation.** The reverse-dependent traversal is
entirely in TSDev.exe-private code (`FUN_004D91E0` and its helpers).

#### 5.2 orlib20 primitives TSDev uses (all available to the CLI)

| Primitive | orlib20 export | RVA | CLI already uses? |
|---|---|---:|---|
| Warehouse singleton | `Ordinal_10` (GetWarehouse, no name) | `0x1A9650` | Yes (resolved, used by delete orchestrator) |
| Master-record lookup by type/name | `Ordinal_28` (no name) | `0x1A03F0` | Not directly (CLI uses `_GetMasterRecByType@16`) |
| Module-record retrieval | `Ordinal_69` (no name) | `0x1A3E00` | No |
| Count accessor (`*(obj+0x0C)`) | `Ordinal_68` (no name) | `0x1A87D0` | No |
| Refcount-dec / free | `Ordinal_70` (no name) | `0x15DF30` | No |
| Global lookup init | `Ordinal_71` (InitGlobalLookup, no name) | `0x1A9460` | Yes (resolved as signature target) |
| Verify (compile) | `_VerifyModule_New@20` (ordinal 700) | `0x169630` | **Yes (core CLI verify)** |
| CELCompiler alloc+ctor | `Ordinal_66` (no name) | `0x165F70` | Yes (M.1) |
| CELCompiler dtor+free | `Ordinal_67` (no name) | `0x165FD0` | Yes (M.1) |
| Type→power-type expansion | `FUN_101d43a0` (internal) | `0x1D43A0` | No (CLI has its own type map) |

All ordinals 10, 28, 68, 69, 70, 71 are **ordinal-only `[NONAME]` exports**
(confirmed via `pefile`: `name=None` for each), resolvable by
`GetProcAddress(hModule, MAKEINTRESOURCEA(ordinal))` — the same mechanism the
CLI already uses for ordinals 66/67 (M.1).

#### 5.3 Historical pre-linker implementation snapshot (superseded by §8.7)

1. **Root verify**: `_VerifyModule_New@20` — the CLI already does this.
2. **Reverse-dependent enumeration**: TSDev builds a `CELPrograms` BST from
   warehouse SafeArray data (command `0x0D` `ProgramById` via the worker
   thread, or cached `InitGlobalLookup` BST). In the historical pre-linker
   snapshot, the CLI did not enumerate reverse dependents. Its existing
   `exists` enumeration (`_LoadMasterRecsEX@16` / `_GetMasterRecsBuf@12`)
   enumerated **all** records of a type, not the reverse-dependent subset.
3. **Contract-change comparison**: `FUN_004d9040` compares two warehouse
   records' metadata fields. The CLI would need to read the old and new
   records (via `Ordinal_28`/`Ordinal_69` or `_GetMasterRecByType@16`) and
   compare the same fields.
4. **Per-dependent recompile**: `_VerifyModule_New@20` per dependent — the
   CLI already has this primitive.
5. **Cycle/visited handling**: TSDev uses a per-record `+0x55` flag, not a
   visited-set. A CLI replication should use an explicit visited-set (safer
   than TSDev's implicit approach).

#### 5.4 (a) Historical export-availability snapshot (superseded as production guidance)

**No.** orlib20.dll exports no "reverse-dependent revalidation" or
"reverse-dependent enumeration" API. The closest exports
(`GetDependencies`, `_GetDynamicDependencyList@12`,
`_GetStaticDependencyList@12`) are **forward** dependency APIs. The
reverse-dependent traversal is TSDev.exe-private.

#### 5.4 (b) Historical composable-primitives analysis (superseded by §8.7)

A minimal behavioral replication of `FUN_004D91E0` using orlib20 primitives:

1. `_VerifyModule_New@20` (ordinal 700) — verify the root (already done by
   the CLI).
2. To enumerate reverse dependents, the CLI would need to either:
   - **(Option B1)** Replicate TSDev's `CELPrograms` BST population: call
     `Ordinal_10` (GetWarehouse) + `Ordinal_71` (InitGlobalLookup) + the
     worker-thread command `0x0D` path (`FUN_004d75b0` equivalent) — but this
     requires driving the orlib20 worker thread, which is internal and not
     exported as a clean enumeration API. This is high-risk coupling to
     internal warehouse mechanics.
   - **(Option B2)** Use the existing master-record enumeration
     (`_LoadMasterRecsEX@16` / `_GetMasterRecsBuf@12`, already used by
     `exists`) to get **all** records of the relevant type(s), then filter to
     those whose **forward dependency list** (via `GetDependencies` /
     `_GetStaticDependencyList@12`) contains the root module's ProgId. This
     is a "reverse by inverting forward" approach: for each candidate record
     R, get R's dependencies; if the root ProgId is in R's dependency list, R
     is a reverse dependent. This uses only **exported** orlib20 APIs.
3. For each reverse dependent, read its record (via `_GetMasterRecByType@16`
   or `Ordinal_28`/`Ordinal_69`) and compare metadata fields per
   `FUN_004d9040` (the field offsets are documented in §2 above).
4. Recurse per dependent with an explicit visited-set (ProgId set) to avoid
   cycles — a **stricter** guard than TSDev's implicit one.
5. Call `_VerifyModule_New@20` per dependent.

#### 5.4 (c) Historical recommendation (superseded by §8.7)

**The original note recommended Option B2 (reverse-by-inverting-forward) using
exported orlib20 APIs**, because:

- It uses only **exported** orlib20 primitives (`GetDependencies` (ord 487),
  `_GetStaticDependencyList@12` (ord 531), `_GetMasterRecByType@16` (ord 510,
  RVA `0x1A0460`), `_LoadMasterRecsEX@16` (ord 568, RVA `0x1A5360`),
  `_GetMasterRecsBuf@12` (ord 512, RVA `0x1A0C70`), `_VerifyModule_New@20`
  (ord 700), `_DeleteDependencyList@4` (ord 397)), all of which are
  resolvable by name (no ordinal-only risk). The export status of the three
  master-record APIs was confirmed by `pefile` (see §8 "PE export-table
  evidence"): all three are **named exports**, not ordinal-only and not
  byte-signature targets.
- It avoids coupling to TSDev.exe-private code (`FUN_004D91E0`) and to
  orlib20 internal worker-thread mechanics (command `0x0D` path).
- It is **stricter** on cycle safety (explicit visited-set) than TSDev's
  implicit `+0x55` flag.
- The contract-change comparison (`FUN_004d9040`'s field offsets) can be
  replicated by reading the two records via `_GetMasterRecByType@16` and
  comparing the documented offsets — this is metadata-only, AGENTS.md
  compliant.

**Do NOT use Option B1** (driving the worker thread / `CELPrograms` BST):
it couples to orlib20 internal mechanics that are not exported and not
stable across versions, violating the "no coupling to TSDev.exe internals"
and "least complexity" principles.

**No direct exported API exists** for the full reverse-dependent
revalidation. The closest single export is `GetDependencies` (forward), which
is a building block for Option B2, not a complete solution.

### Historical answer (R-M.4.5; superseded by the current linker/ordinal path)

- **(a) Historical finding:** No. orlib20.dll does not export a reverse-dependent
  revalidation or reverse-dependent enumeration API. The reverse-dependent
  traversal is TSDev.exe-private (`FUN_004D91E0`). The dependency exports
  (`GetDependencies`, `_GetDynamicDependencyList@12`,
  `_GetStaticDependencyList@12`) are **forward** only.
- **(b) Historical composition:** The original analysis composed the traversal
  from exported orlib20 primitives: `_VerifyModule_New@20` (verify),
  `GetDependencies`/`_GetStaticDependencyList@12` (forward deps, to invert),
  `_GetMasterRecByType@16` (record reads), and `_DeleteDependencyList@4`
  (cleanup). The contract-change check was replicated by comparing the
  documented metadata field offsets.
- **(c) Historical recommendation (superseded):** The original note recommended
  **Option B2** (reverse-by-inverting-forward, using exported APIs + an explicit
  visited-set). It was retained for its historical RE value, but is not the
  current production path; see §8.7 for the linker (ordinal 85) approach.

---

## 6. Historical Synthesis & Implementation Recommendation (superseded by §8.7)

### What the original plan proposed for replicating `FUN_004D91E0`

1. **Root verify**: continue using `_VerifyModule_New@20` (ordinal 700) —
   already implemented.
2. **Reverse-dependent enumeration**: implement Option B2 —
   enumerate all records of the root's category (via the existing
   `_LoadMasterRecsEX@16` (ord 568, RVA `0x1A5360`) /
   `_GetMasterRecsBuf@12` (ord 512, RVA `0x1A0C70`) enumeration used by
   `exists` — both confirmed named exports, see §8), and for each
   candidate, call `GetDependencies` (ordinal 487) or
   `_GetStaticDependencyList@12` (ordinal 531) to get its forward dependency
   list; if the root ProgId appears in that list, the candidate is a reverse
   dependent. Free the dep list via `_DeleteDependencyList@4` (ordinal 397).
3. **Contract-change check (ROOT, done once)**: compare the ROOT's old
   warehouse record (captured before `saveSource`) vs the ROOT's new record
   (read after root verify succeeds, via `_GetMasterRecByType@16` (ord 510,
   RVA `0x1A0460`) — confirmed named export, see §8) using the metadata
   fields documented in §2 (`+0xd6`, `+0x15`, `+0x7f`, `+0x81`, `+0x29`,
   `+0x4f` array with stride `0xb5`, element fields `+0xd`/`+0x11`/`+0x15`/
   `+0x83`/`+0x87`/`+0xb1`). This is metadata-only, AGENTS.md compliant.
   The result gates the traversal: if the contract changed, reverse
   dependents are revalidated; if unchanged, verified dependents are skipped
   (CORRECTED — original text said the check was per-dependent and inverted
   the gate; per §1.6, the check is done once for the root, and per §3.3,
   the gate skips on verified AND not-changed).
4. **Per-dependent recompile**: call `_VerifyModule_New@20` for each reverse
   dependent (full recompile, matching TSDev).
5. **Cycle safety**: use an **explicit visited-set** of ProgIds (stricter
   than TSDev's `+0x55` flag). Skip already-visited dependents.
6. **Failure handling**: AND all verify results into an aggregate; the root
   result (`+0x00`) drives success/failure (matching TSDev — confirmed in
   §4.3 that `FUN_00451800` branches on `+0x00`, and `+0x04` is not consumed
   by the immediate UI path). Collect per-dependent compile errors via the
   existing compiler error machinery.
7. **Gating**: only run the reverse-dependent traversal for categories 0
   (Strategy), 1 (Function), 4 (SystemFunction), matching TSDev's gate at
   `0x004d92b2`–`0x004d92bd`.

### Whether a direct exported API exists

**No.** See §5.4(a). The reverse-dependent revalidation must be composed
from exported primitives; there is no single orlib20 export that performs it.

### Key parity note

The prior RE conclusion (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md` §F
("Disproved or non-causal hypotheses")) that "reverse-dependent revalidation
is not required for root linkability" is
**consistent** with these findings: `FUN_004D91E0`'s root verify
(`_VerifyModule_New@20`) is the linkability-critical step; the
reverse-dependent cascade is a propagation step for already-running
consumers, not a root-link requirement. The CLI's current single-module
verify is linkable; reverse-dependent revalidation is a **parity** feature
(M.4), not a linkability fix (Workstream L).

---

## 7. Gaps / Open Questions

1. **Cycle safety — dynamic confirmation needed.** Static analysis shows no
   explicit visited-set; cycle protection is implicit (per-record `+0x55`
   flag + contract-change gate). Whether the orlib20 warehouse can ever
   contain a cycle in the reverse-dependent set is **not proven statically**.
   A dynamic Frida trace with a synthetic A→B→A dependency cycle would
   confirm whether the recursion terminates. (Recommendation: the CLI's
   replication should use an explicit visited-set regardless, which is
   strictly safer.)

2. **`+0x18` field overload.** `FUN_004D91E0` temporarily overwrites
   `compiler+0x18` (normally the HWND per M.1) with a record-derived value
   (`*(record + 0xe4)`) during the reverse-dependent loop, and passes it as
   the first arg (HWND) to `_VerifyModule_New@20` in the recursive call.
   Static analysis cannot fully determine whether `+0xe4` is a notification
   handle, a record pointer that orlib20 tolerates as HWND, or a genuine
   ProgId-as-HWND. This does not affect the CLI replication (which uses its
   own HWND), but is noted for completeness. A dynamic trace capturing the
   `+0x18` value and the `_VerifyModule_New@20` first-arg during a
   reverse-dependent verify would resolve this.

3. **`CELPrograms` BST population source.** `FUN_004c9670`/`FUN_004cad90`
   populates the BST from a SafeArray at `*(record + 0x26)`. The exact
   command/COM method that produces this SafeArray for the reverse-dependent
   set (vs. the forward `GetDependencies` path) was not fully traced. The
   worker-thread command `0x0D` (`ProgramById`) is used by `FUN_004d75b0`,
   but how `ProgramById` returns a **set** of reverse dependents (rather than
   a single record) is not fully resolved statically. This gap did not block the
   historical Option B2 analysis (which used forward-dep inversion); the current
   production path is the linker-based approach in §8.7.

4. **Contract-change field semantics.** The exact semantic meaning of each
   compared field in `FUN_004d9040` (e.g., what `+0x15` bit 2 represents) is
   not labeled in the binary. The offsets and comparison logic are exact, but
   the field names are inferred. A dynamic trace correlating a known
   contract change (e.g., adding a parameter to a Function) with the bit
   flips would label the fields. The CLI replication can compare the same
   offsets without knowing the semantic names.

5. **No TSDev UI reverse-traversal trace was captured for the original M.4
   slice.** The §8 runtime diagnostic is not a TSDev UI trace; it records
   linker/object-code behavior and does not establish a current acceptance
   result. A Frida trace of a UI Verify that triggers reverse-dependent
   revalidation (e.g., modifying a Function that a Strategy calls, then
   verifying the Function) would confirm: (a) the recursion order, (b) the
   `+0x55` flag behavior, (c) the `+0x18` overload, and (d) the
   failure-reporting aggregate. This remains a recommendation, not a blocker
   for the static findings.

6. **`compiler+0x04` (dependent-failure aggregate) downstream consumption
   not exhaustively traced.** §4.3 confirms — by decompiling `FUN_00451800`
   and its immediate callees (`FUN_0044a1e0`, `FUN_0044c410`,
   `FUN_0044b880`, `FUN_004502f0`, `Ordinal_67`) — that none read
   `compiler+0x04` after the success/failure branch, so the aggregate is
   dead with respect to the immediate UI path. Whether `+0x04` is consumed
   by any deeper downstream UI/reporting path beyond those immediate
   callees (e.g., inside the virtual `+0x1f8` success callback on the
   CDocument, or inside `_GetCompilingErrors_4`'s callees) was not
   exhaustively traced statically. This does not affect the M.4.4 conclusion
   for the `FUN_00451800` branch (which is provably on `+0x00`), but is
   recorded for completeness. A dynamic trace reading `compiler+0x04` after
   `FUN_004d9480` returns would close this gap.

---

## 8. Evidence Index

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

- `pefile` scan of `C:\Program Files (x86)\TradeStation 9.5\Program\orlib20.dll`:
  ordinal base = 10; ordinals 10, 28, 66, 67, 68, 69, 70, 71 are all
  `[NONAME]` (ordinal-only) at the RVAs listed above.
  Dependency-named exports: ordinals 487 (`GetDependencies`), 489
  (`_GetDynamicDependencyList@12`), 531 (`_GetStaticDependencyList@12`),
  396/397 (`_DeleteChangedModulesList@4`/`_DeleteDependencyList@4`).
- Master-record enumeration exports (confirmed named, resolvable by
  `GetProcAddress(hModule, "<name>")` — no ordinal-only risk):
  - ordinal **510** `_GetMasterRecByType@16`, RVA `0x1A0460`
    (`0x101A0460`)
  - ordinal **568** `_LoadMasterRecsEX@16`, RVA `0x1A5360`
    (`0x101A5360`)
  - ordinal **512** `_GetMasterRecsBuf@12`, RVA `0x1A0C70`
    (`0x101A0C70`)
  All three are **named exports** (verified via `pefile`: `e.name` is
  non-`None` for each). The historical Option B2 recommendation (§5.4(c), §6)
  relied on these named exports plus `GetDependencies`/
  `_GetStaticDependencyList@12`/`_DeleteDependencyList@4`/
  `_VerifyModule_New@20`; that recommendation is superseded by the
  linker-based production path documented in §8.7.

### Prior RE cited

- `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md` §6 (Verify chain static evidence)
- `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md` §F ("Disproved or non-causal
  hypotheses") — +0x10 / outer recursive wrapper and the finding that
  reverse-dependent revalidation is not required for root linkability
- `docs/RE/RE_M3_0x40A_NOTIFICATION.md` (format pattern + GetWarehouse RVA
  `0x1A9650` cross-reference)
- `docs/M1_HANDOFF.md` §R-M.1.2 (compiler field layout: +0x08 category,
  +0x0C subtype, +0x10 ProgId, +0x18 HWND)
- `docs/SIGNATURE_CATALOG.md` §4.7 (GetWarehouse RVA `0x1A9650`), §5
  (ordinals 66/67 resolution mechanism)

---

## 8. GetDependencies Type-Code Mismatch and Linker-Based Dep-List Approach

**Date:** 2026-08-26
**Method:** Ghidra static analysis (agent c6d57810) + runtime diagnostic `diag_m4_deplist_dump` (agent 6f30a931)

### 8.1 GetDependencies is broken: type-code mismatch (0xFA4 vs 0xFA3)

`FUN_10153ad0` (the GetDependencies internal helper) checks for dep-list entry type `0xFA4` to identify function dependencies. However, the ACTUAL function-dependency entry type written by `MakeSymbol` (RVA 0x10021800) during compilation is `0xFA3`, not `0xFA4`. This was confirmed by runtime diagnostic `diag_m4_deplist_dump`:

- 7 `0xFA3` entries found in 2 existing Indicators (ProgId=3 and ProgId=8)
- Zero `0xFA4` entries found across all 25 tested items (10 Strategies, 10 Indicators, 5 Functions)
- GetDependencies returns empty for ALL items because it checks for the wrong type code

This is the REAL root cause of GetDependencies returning empty — not a field mismatch (the earlier "record+0x6f vs element+0x7b" theory was incorrect; record+0x6f is a compilation working buffer, not a dep-list).

### 8.2 record+0x6f is NOT a dependency list

The buffer at `record+0x6f` (size at `record+0x73`) is a **64KB compilation working buffer**, not a dependency list. Three independent readers confirm this:

1. `GetTokenOnlyInfo` (0x1001f310): reads record+0x6f, searches for it in a global array at 0x1023db14, then frees or releases it.
2. `FUN_10151550` (record destructor): same pattern — searches global array, frees/releases.
3. `FUN_101158d0` (LinkModules): reads record+0x6f as compiled tokens (compares first byte with 0x8a).

The `9c 29 00 00` "magic" at the start of the buffer is just the first bytes of compiled token data, not a structured header. The `00 04` and `00 37` are compiled token data, not category/ProgId fields.

### 8.3 The actual dep-lists are at element+0x7b (per-element)

The real dependency lists are stored per-element:
- `element+0x7b` = dep-list buffer pointer
- `element+0x73` = dep-list byte size

Element stride is `0xb5` (181 bytes). The element array is at `record+0x4f` (pointer) with count at `record+0x29`.

### 8.4 Dep-list entry format

Each entry in the dep-list has this layout:
- `+0x00` (4 bytes): type (dword) — e.g., 0xFA3 for function dependency
- `+0x04` (1 byte): total entry length (used as iteration stride)
- `+0x05` (4 bytes): source line
- `+0x09` (4 bytes): source column
- `+0x0d` (4 bytes): payload — for 0xFA3/0xFA4 entries, this is the **dependent Function's ProgId** (int)

Entry iteration: advance by the length byte at entry+4. Continue while `(cursor - base) < depListSize`.

### 8.5 The function reference at entry+0xd IS a ProgId

Ghidra analysis of `MakeSymbol` (RVA 0x10021800) confirms that the value written at entry+0xd is `(int)(param_9 + 0.5)` — a float-to-int round of the symbol's numeric value. The very next line passes this same int to `GetFunctionReturnType(1, progId)`, which resolves it via BST lookup keyed by ProgId (outer tree `std::map<int ProgId, InnerTree>`, node key at +0x10). Therefore the value at entry+0xd IS the warehouse ProgId of the dependent Function.

`ExpandFuncList` (0x1001c890) is a trivial dedup-collect utility (static method, no `this` needed). It does NOT resolve the ProgId — it just appends it to a GlobalAlloc'd array. No resolution step is needed; the value at entry+0xd can be directly compared to the modified Function's ProgId.

### 8.6 Category-specific dep-list presence

Runtime diagnostic confirmed:
- **Strategies (cat 0):** dep-lists exist (size=38 bytes, 2 entries per element). First 10 tested had only 0xBB8/0xBB9 entries (no function calls). Strategies that call Functions would have 0xFA3 entries.
- **Indicators (cat 2):** dep-lists exist (size 35/38/80). 0xFA3 entries found in items that call Functions.
- **Functions (cat 4):** dep-lists are empty (size=0, ptr=NULL for all elements), observed in the sampled existing Functions; design intent is unconfirmed.

### 8.7 M.4 implementation approach (linker-based, bypasses GetDependencies)

Since GetDependencies is broken (type-code mismatch), M.4 must enumerate reverse dependents by:

1. For each candidate item (in all categories), call the linker (ordinal 85) to get the linked record
2. Read the element array (record+0x4f/+0x29, stride 0xb5)
3. For each element, read the dep-list (element+0x7b/+0x73)
4. Parse entries: check for type 0xFA3 AND 0xFA4 (both have ProgId at entry+0xd)
5. If any entry's ProgId matches rootProgId, that item is a reverse dependent

This bypasses the broken GetDependencies entirely. The linker call requires a partial record buffer (obtained via Ordinal_28) and the record must be released via Ordinal_70 after use.

### 8.8 Known limitation

This approach only works for items where the linker can populate element arrays (existing TSDev-created items). CLI-compiled items have empty element arrays due to the separate CLI-created object-code defect (version=0, element count=0), which is tracked by M.3b. M.4 remains blocked until that defect is resolved; this is not an M.4 acceptance result.

