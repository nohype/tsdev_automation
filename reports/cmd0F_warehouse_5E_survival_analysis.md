# cmd 0x0F Dispatcher (FUN_101d0950) -- `warehouse+0x5E` Survival Analysis

**Project:** TSDevAnalysis / `orlib20.dll` (base `0x10000000`)
**Scope:** READ-ONLY Ghidra investigation. No files modified.
**Question:** After `FUN_101d0950` (RVA `0x1d0950`, the specialized cmd 0x0F dispatcher) returns, is `warehouse+0x5E` still valid (pointing to the loaded `IProgram`)?

---

## Step 1 -- `FUN_101d0950` decompilation + `+0x5E` analysis

### 1.1 Full decompilation (Ghidra pseudocode)

```c
undefined4 __thiscall FUN_101d0950(int param_1, undefined4 param_2,
                                   undefined4 param_3, int param_4)
{
  code *pcVar1;
  undefined4 uVar2;
  DWORD DVar3;

  if (*(int *)(param_1 + 0xae) != 0) {
    *(undefined2 *)(param_1 + 0x1e) = 0x2000;
    uVar2 = FUN_101d1010(param_2);
    *(undefined4 *)(param_1 + 0x26) = uVar2;
    *(undefined4 *)(param_1 + 0x12) = param_3;
    WaitForSingleObject(*(HANDLE *)(param_1 + 0x72), 0xffffffff);
    DVar3 = GetCurrentThreadId();
    if (DVar3 != *(DWORD *)(param_1 + 0x76)) {
      pcVar1 = (code *)swi(3);
      uVar2 = (*pcVar1)();
      return uVar2;
    }
    *(undefined4 *)(param_1 + 4) = 0xf;                 // cmd = 0x0F (LoadProgram)
    SetEvent(*(HANDLE *)(param_1 + 0x6e));              // wake worker thread
    WaitForSingleObject(*(HANDLE *)(param_1 + 0x6a), 0xffffffff);  // wait for worker
    ReleaseMutex(*(HANDLE *)(param_1 + 0x72));
    SafeArrayDestroy(*(SAFEARRAY **)(param_1 + 0x26));
    if (*(int *)(param_1 + 0x4e) == 0) {                // worker success
      FUN_101c4ab0(param_1, *(undefined4 *)(param_1 + 0x5e));  // <-- READS +0x5E
      *(undefined4 *)(param_4 + 0x26d) = 1;
      return 1;
    }
  }
  return 0;
}
```

### 1.2 Disassembly of the `+0x5E` touch point (verified -- the pseudocode hides a `this` swap)

The decompiler's rendering of the `FUN_101c4ab0` call is misleading. The raw
instructions at `0x101d09c7`-`0x101d09d5` show what really happens:

```asm
101d09c1  CMP  dword ptr [ESI + 0x4e], 0x0      ; worker success check
101d09c5  JNZ  0x101d09e9
101d09c7  PUSH dword ptr [ESI + 0x5e]            ; arg3 = warehouse+0x5E (IProgram)
101d09ca  PUSH ESI                               ; arg2 = warehouse (this of FUN_101d0950)
101d09cb  MOV  ESI, dword ptr [EBP + 0x10]       ; ESI = param_4 (the prog_writer)
101d09ce  MOV  ECX, ESI                          ; ECX = prog_writer  (__thiscall this)
101d09d0  CALL 0x101c4ab0                        ; FUN_101c4ab0(prog_writer, warehouse, IProgram)
101d09d5  MOV  dword ptr [ESI + 0x26d], 0x1      ; prog_writer+0x26D = 1
```

So `FUN_101c4ab0` is invoked with:
- `this` (ECX) = `param_4` of `FUN_101d0950` -- **the program-writer object**, NOT the warehouse
- stack arg 1 = `ESI` = warehouse (`param_1` of `FUN_101d0950`)
- stack arg 2 = `[ESI+0x5e]` = the IProgram pointer just loaded by the worker

### 1.3 Answers to the Step 1 sub-questions

| # | Question | Answer | Evidence |
|---|----------|--------|----------|
| 1 | Does `FUN_101d0950` SET `warehouse+0x5E`? | **No.** It only **reads** `[ESI+0x5e]` once, at `0x101d09c7`, to pass it to `FUN_101c4ab0`. The actual SET of `+0x5E` is done by the worker thread while `FUN_101d0950` is blocked on `WaitForSingleObject([ESI+0x6a])` (`0x101d09a9`). `FUN_101d0950` never writes to `+0x5E` itself. | disasm `0x101d09c7` (read-only `PUSH [ESI+0x5e]`); no `MOV [ESI+0x5e], ...` anywhere in the function |
| 2 | Does `FUN_101d0950` CLEAR `warehouse+0x5E` (write 0/NULL)? | **No.** There is no instruction in `FUN_101d0950` that writes to `[ESI+0x5e]`. The only writes to the warehouse are: `+0x1e` (cmd flags `0x2000`), `+0x26` (SafeArray), `+0x12` (param_3), `+0x4` (cmd id `0x0F`). | full disassembly (46 insns) -- no `[ESI+0x5e]` store |
| 3 | Does `FUN_101d0950` call `FUN_101c69b0` (cmd 0x4F/Release, which clears `+0x5E`)? | **No.** The only `CALL` targets are: `FUN_101d1010` (`0x101d096f`), `FUN_101c4ab0` (`0x101d09d0`), and Win32 imports `WaitForSingleObject`, `GetCurrentThreadId`, `SetEvent`, `ReleaseMutex`, `SafeArrayDestroy`. | disasm call list |
| 4 | Does `FUN_101d0950` call `FUN_101c4ab0`? What does it do to `+0x5E`? | **Yes**, at `0x101d09d0`. `FUN_101c4ab0` **reads** `warehouse+0x5E` (passed as arg2) and **copies** it into `prog_writer+0x10`. It does **not** clear `warehouse+0x5E`. See Step 2. | disasm `0x101d09c7`-`0x101d09d0`; Step 2 |
| 5 | State of `warehouse+0x5E` after `FUN_101d0950` returns to its caller | **Still non-NULL -- pointing to the loaded IProgram.** `FUN_101d0950` neither writes nor clears `+0x5E`; it only reads it to hand the pointer to `FUN_101c4ab0`, which copies it out without zeroing the source. | Steps 1.3.1-1.3.4 + Step 2 |

---

## Step 2 -- `FUN_101c4ab0` (RVA `0x1c4ab0`) analysis

### 2.1 Decompilation

```c
void __thiscall FUN_101c4ab0(int *param_1, int param_2, int param_3)
{
  (**(code **)(*param_1 + 4))();     // prog_writer->vtable[1]()  (no args)
  (**(code **)(*param_1 + 0xc))();   // prog_writer->vtable[3]()  (no args)
  param_1[3] = param_2;              // prog_writer + 0x0C = warehouse
  param_1[4] = param_3;              // prog_writer + 0x10 = IProgram (from warehouse+0x5E)
  return;
}
```

### 2.2 Resolved argument mapping (from the call site at `0x101d09c7`)

| Callee param | Register/stack | Actual value (from `FUN_101d0950`) |
|--------------|----------------|-------------------------------------|
| `param_1` (`this`, ECX) | `ECX` | `param_4` of `FUN_101d0950` = **prog_writer** |
| `param_2` (stack) | `[esp]` | `ESI` = **warehouse** (`param_1` of `FUN_101d0950`) |
| `param_3` (stack) | `[esp+4]` | `[warehouse+0x5e]` = **IProgram** |

### 2.3 Answers to the Step 2 sub-questions

1. **Does it read `warehouse+0x5E`?**
   **Yes -- indirectly.** `FUN_101d0950` reads `[warehouse+0x5e]` at `0x101d09c7` and pushes it as `param_3`. Inside `FUN_101c4ab0`, `param_3` is consumed by `param_1[4] = param_3;` (the copy to `prog_writer+0x10`). `FUN_101c4ab0` itself never dereferences the warehouse pointer; it only receives the IProgram value by value.

2. **Does it clear `warehouse+0x5E`?**
   **No.** `FUN_101c4ab0` has no write to `param_2`-relative memory and no write to any `+0x5e` offset. It writes only to `param_1[3]` (`prog_writer+0x0C`) and `param_1[4]` (`prog_writer+0x10`). The warehouse (`param_2`) is stored into `prog_writer+0x0C` as a back-pointer but is never modified.

3. **Does it copy the IProgram to a program-writer object at `prog_writer+0x10`?**
   **Yes -- exactly.** `param_1[4] = param_3;` writes the IProgram (sourced from `warehouse+0x5E`) to `prog_writer + 0x10`. This is the "loaded program" slot inside the program-writer. It also stores the warehouse back-pointer at `prog_writer + 0x0C` (`param_1[3] = param_2`).

4. **Does it leave `warehouse+0x5E` intact or zero it?**
   **It leaves `warehouse+0x5E` intact.** There is no zeroing. The two virtual calls (`prog_writer->vtable[1]()` and `vtable[3]()`) operate on `prog_writer` only (no args, `this = prog_writer`); they do not touch the warehouse.

---

## Step 3 -- Cleanup-chain analysis (caller responsibility)

### 3.1 Does `FUN_101d0950` call `FUN_101c0fd0` (program-writer reset) internally?

**No.** Confirmed by the full disassembly (Step 1.3.3 table): the only internal call
targets are `FUN_101d1010` and `FUN_101c4ab0`. `FUN_101c0fd0` is **not** invoked.

### 3.2 Does `FUN_101d0950` call `FUN_101c00c0`?

**No.** Same evidence -- `FUN_101c00c0` does not appear in the call list.

### 3.3 Does `FUN_101d0950` just load the program and return, leaving cleanup to the caller?

**Yes -- exactly that.** `FUN_101d0950` is a thin "dispatch cmd 0x0F to the worker
thread, wait, then copy the resulting IProgram into the caller-supplied prog_writer"
shim. It performs **no** program-writer teardown and **no** `+0x5E` cleanup.

### 3.4 Proof: the canonical caller `CompileText` does the cleanup itself

`FUN_101d0950` has 9 in-program callers (xrefs to `0x101d0950`); the named one is
`CELBasicCompiler::CompileText` at `0x1001dd80`. Its body shows the canonical
post-`0x0F` cleanup sequence (decompiled at `0x1001dd80`):

```c
iVar3 = FUN_101d0950(local_1dc, param_3, local_458);   // local_458 = prog_writer
if (iVar3 == 1) {
  FUN_101c7c60((byte)pIVar8[0x82] & 1);
}
...
local_458[0] = CELProgramWriter::vftable;              // (re)init prog_writer vtable
FUN_101c0fd0();                                         // <-- RESET  (clears +0x5E)
FUN_101c00c0();                                         // <-- DTOR   (clears +0x5E again)
```

So in the full `CompileText` path, the **caller** -- not `FUN_101d0950` -- invokes
both `FUN_101c0fd0` and `FUN_101c00c0` on the prog_writer (`local_458`) after
`FUN_101d0950` returns.

### 3.5 The cleanup chain that ultimately clears `+0x5E`

Decompiling the chain confirms the prior analysis:

**`FUN_101c0fd0` (`0x101c0fd0`) -- program-writer reset:**
```c
void __fastcall FUN_101c0fd0(int param_1) {
  if (*(int *)(param_1 + 0x241) == 1) FUN_101c1590();
  *(undefined4 *)(param_1 + 0x245) = 0;
  ... (zeros +0x245..+0x271) ...
  FUN_101c0ca0();                  // <-- descends into the +0x5E clearer
}
```

**`FUN_101c0ca0` (`0x101c0ca0`) -- program-writer teardown core:**
```c
... (frees strings, arrays, handles at +0x1d, +0x21, +0x6a, +0x75, +0x7a, ...) ...
if (*(int *)(param_1 + 0x10) != 0) {     // prog_writer+0x10 (IProgram copy) non-NULL?
  FUN_101c69b0();                        // <-- the +0x5E clearer
}
...
```

**`FUN_101c69b0` (`0x101c69b0`) -- cmd 0x4F / Release (the actual `+0x5E` clearer):**
```c
void __fastcall FUN_101c69b0(int param_1) {
  CSingleLock::CSingleLock(local_1c, *(CSyncObject **)(param_1 + 0x218), 1);
  FUN_101c82b0();
  if (*(int *)(param_1 + 0x10) != 0) {                          // prog_writer+0x10 (IProgram)
    *(int *)(*(int *)(param_1 + 0xc) + 0x5e) =                  // warehouse+0x5E =
        *(int *)(param_1 + 0x10);                               //   prog_writer+0x10  (re-arm)
    FUN_101cf5b0(0x4f);                                         // dispatch cmd 0x4F (Release)
    *(undefined4 *)(*(int *)(param_1 + 0xc) + 0x5e) = 0;        // *** warehouse+0x5E = 0 ***
    *(undefined4 *)(param_1 + 0x10) = 0;                        // prog_writer+0x10 = 0
  }
  CSingleLock::Unlock(local_1c);
}
```

Note `param_1 + 0xc` is the warehouse back-pointer that `FUN_101c4ab0` stored at
`prog_writer+0x0C`. So `FUN_101c69b0` re-arms `warehouse+0x5E` with the IProgram,
dispatches cmd 0x4F (Release) on the worker, then **zeros `warehouse+0x5E`** and
zeros `prog_writer+0x10`.

**`FUN_101c00c0` (`0x101c00c0`) -- `CELProgram` destructor:**
Also calls `FUN_101c0ca0()` (same chain -> `FUN_101c69b0` -> clears `+0x5E`). By the
time it runs in `CompileText`, `+0x5E` is already 0 from `FUN_101c0fd0`, so the
`if (*(int *)(param_1 + 0x10) != 0)` guard skips the re-clear.

### 3.6 Summary of the cleanup chain

```
Caller (CompileText / callCmd0FDispatcher path)
   |
   |-- FUN_101d0950(warehouse, ..., prog_writer)        [does NOT clear +0x5E]
   |      `-- FUN_101c4ab0(prog_writer, warehouse, [warehouse+0x5e])
   |              `-- prog_writer+0x0C = warehouse
   |              `-- prog_writer+0x10 = IProgram      [+0x5E left intact]
   |
   |-- FUN_101c0fd0(prog_writer)                       [CALLER does this]
   |      `-- FUN_101c0ca0(prog_writer)
   |             `-- if (prog_writer+0x10 != 0)
   |                    `-- FUN_101c69b0(prog_writer)
   |                           |-- warehouse+0x5E = prog_writer+0x10   (re-arm)
   |                           |-- FUN_101cf5b0(0x4F)                  (Release)
   |                           |-- warehouse+0x5E = 0   *** CLEARED ***
   |                           `-- prog_writer+0x10 = 0
   |
   `-- FUN_101c00c0(prog_writer)                       [CALLER does this]
          `-- FUN_101c0ca0(prog_writer)  (guard skips; +0x10 already 0)
```

---

## Step 4 -- Definitive answer

### 4.1 Does `warehouse+0x5E` survive after `FUN_101d0950` returns (with no subsequent cleanup)?

**YES -- `warehouse+0x5E` remains non-NULL and points to the loaded `IProgram`.**

Evidence:
- `FUN_101d0950` (`0x101d0950`) **never writes** to `[warehouse+0x5e]`. The full
  46-instruction disassembly contains no `MOV [ESI+0x5e], ...` store.
- The only touch of `+0x5E` is a **read** at `0x101d09c7` (`PUSH dword ptr [ESI+0x5e]`),
  feeding it as `param_3` to `FUN_101c4ab0`.
- `FUN_101c4ab0` (`0x101c4ab0`) copies that value into `prog_writer+0x10`
  (`param_1[4] = param_3;`) and stores the warehouse back-pointer at `prog_writer+0x0C`
  (`param_1[3] = param_2;`). It performs **no** write to `warehouse+0x5E`.
- `FUN_101d0950` does **not** call any of the `+0x5E`-clearing functions
  (`FUN_101c69b0`, `FUN_101c0ca0`, `FUN_101c0fd0`, `FUN_101c00c0`). Its only
  internal calls are `FUN_101d1010` and `FUN_101c4ab0`.

Therefore, immediately after `FUN_101d0950` returns:
- `warehouse+0x5E` -> still the loaded `IProgram*` (non-NULL)
- `prog_writer+0x10` -> copy of the same `IProgram*`
- `prog_writer+0x0C` -> warehouse back-pointer

### 4.2 If the caller invokes `FUN_101c0fd0` / `FUN_101c00c0` afterwards, does that clear `+0x5E`?

**YES -- both do, via `FUN_101c0ca0 -> FUN_101c69b0`.**

- `FUN_101c0fd0` (`0x101c0fd0`) -> `FUN_101c0ca0` (`0x101c0ca0`) -> guards on
  `prog_writer+0x10 != 0` -> `FUN_101c69b0` (`0x101c69b0`) -> re-arms
  `warehouse+0x5E`, dispatches cmd 0x4F (Release) via `FUN_101cf5b0(0x4F)`, then
  executes `*(undefined4 *)(warehouse + 0x5e) = 0;` (the clearing store, confirmed
  in the decompilation at `0x101c69b0`).
- `FUN_101c00c0` (`0x101c00c0`) -> same `FUN_101c0ca0` path; the
  `prog_writer+0x10 != 0` guard is false by then (already zeroed by
  `FUN_101c69b0`), so it is a no-op for `+0x5E` but completes `CELProgram`
  destruction.

This is exactly the sequence the canonical caller `CELBasicCompiler::CompileText`
(`0x1001dd80`) performs:
```c
local_458[0] = CELProgramWriter::vftable;
FUN_101c0fd0();   // clears warehouse+0x5E
FUN_101c00c0();   // CELProgram dtor (no-op for +0x5E by now)
```

### 4.3 Is there a way to call `FUN_101d0950` and dispatch cmd 0x3B BEFORE any cleanup clears `+0x5E`?

**YES -- and the window is wide open.** Because `FUN_101d0950` itself does **not**
run the cleanup chain, the cleanup is entirely the caller's responsibility. The
ordering constraint is:

```
FUN_101d0950(...)          <- +0x5E becomes valid (worker sets it; FUN_101d0950 leaves it)
   v
(cmd 0x3B dispatch here)   <- +0x5E still valid -- IProgram::Callers can run
   v
FUN_101c0fd0 / FUN_101c00c0  <- ONLY HERE does +0x5E get cleared
```

Concretely, for the CLI's `callCmd0FDispatcher` path:
1. `callCmd0FDispatcher` calls `FUN_101d0950(warehouse, ..., prog_writer)`.
   On return, `warehouse+0x5E` is non-NULL (loaded `IProgram`).
2. **If `callCmd0FDispatcher` does NOT call `FUN_101c0fd0` / `FUN_101c00c0`
   afterwards**, `+0x5E` stays valid indefinitely (until the warehouse itself is
   destroyed or cmd 0x4F is dispatched some other way).
3. **If `callCmd0FDispatcher` DOES call `FUN_101c0fd0` / `FUN_101c00c0`**
   (mirroring `CompileText`), `+0x5E` is cleared by `FUN_101c69b0` during that
   cleanup -- so cmd 0x3B must be dispatched **between** the `FUN_101d0950` return
   and the first cleanup call.

**Recommended sequence for cmd 0x3B after a cmd 0x0F load via `callCmd0FDispatcher`:**

```
callCmd0FDispatcher(...)        // FUN_101d0950 returns; warehouse+0x5E valid
   v
dispatch cmd 0x3B (IProgram::Callers)   // uses warehouse+0x5E
   v
(only now) call FUN_101c0fd0 / FUN_101c00c0 cleanup   // clears +0x5E
```

The critical invariant: **`FUN_101d0950` is safe to use as the loader; it does not
self-clean. The cmd 0x3B dispatch must simply occur before whatever cleanup the
caller chooses to run.** If `callCmd0FDispatcher` is structured to call cmd 0x3B
*before* any `FUN_101c0fd0`/`FUN_101c00c0` teardown, `+0x5E` will be valid for the
`IProgram::Callers` query.

---

## Evidence index (all addresses in `orlib20.dll`, abs = `0x10000000 + RVA`)

| Function | Address | Role | Touches `+0x5E`? |
|----------|---------|------|-------------------|
| `FUN_101d0950` | `0x101d0950` | cmd 0x0F dispatcher (specialized) | Reads only (`0x101d09c7`); never writes/clears |
| `FUN_101c4ab0` | `0x101c4ab0` | copy IProgram -> `prog_writer+0x10`, warehouse -> `prog_writer+0x0C` | Reads (via arg); does NOT clear `+0x5E` |
| `FUN_101c0fd0` | `0x101c0fd0` | program-writer reset (caller-side cleanup) | Indirectly clears via `FUN_101c0ca0 -> FUN_101c69b0` |
| `FUN_101c0ca0` | `0x101c0ca0` | program-writer teardown core | Calls `FUN_101c69b0` if `prog_writer+0x10 != 0` |
| `FUN_101c69b0` | `0x101c69b0` | cmd 0x4F / Release | **Clears `warehouse+0x5E = 0`** (the actual zeroing store) |
| `FUN_101c00c0` | `0x101c00c0` | `CELProgram` destructor (caller-side cleanup) | Same chain as `FUN_101c0fd0` (no-op for `+0x5E` if already cleared) |
| `CELBasicCompiler::CompileText` | `0x1001dd80` | canonical caller; demonstrates the cleanup pattern | Calls `FUN_101c0fd0` + `FUN_101c00c0` after `FUN_101d0950` |
