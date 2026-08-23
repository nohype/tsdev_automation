# RE: `_VerifyModule_New` Finalization Path — ELL/XML Truncation on Failed Modify

**Status:** COMPLETE
**Date:** 2026-08-23
**TVS:** L.4 (adversarial review finding — modify-rollback characterization)
**Binary analyzed:** `orlib20.dll` (image base `0x10000000`), plus `ELMDCore.dll`, `ELDebugger.dll`, `ECL.dll` (loaded into Ghidra project `TSDevAnalysis`)
**Tool:** Ghidra MCP (static decompilation, cross-references, vtable resolution)

> **Workstream L is NOT YET CLOSED — see `docs/PM_WORKING_MEMORY.md`.** The TVS-L.4/L.5 completion claim was FALSIFIED by persistence and deletion fault injection and is **superseded by the L.6–L.15 recovery**, which is **implemented on branch `workstream-L-recovery`** (L.1–L.3 remain COMPLETE; L.4/L.5 are NOT complete — their DONE text is retained as dated/superseded; L.8 BLOCKED — no invented TSDev recovery; L.9 detect-and-fail stale-ELL modify/upsert; L.12 detect-and-report orphan-ELL delete; existence checks are necessary but not sufficient; the `FUN_10011230` persistence bypass is removed). **No new Desktop runtime re-gate is claimed without an L.17 result.** Workstream M remains BLOCKED until L.17 independent final verification. The analysis body below is unchanged and remains correct for the finalization-truncation scenario it analyzes; only this current-truth pointer banner is updated.

> **CORRECTION (2026-08-23):** The original version of this report swapped the .ell and .xml file extensions throughout. The corrected mapping (proven by L.3 dynamic A/B evidence) is: SymWriter::Save writes .xml (XML symbol file), MDVault::SaveModule writes .ell (binary metadata). The original conclusion that .ell can be truncated on a failed modify was WRONG. In the finalization-truncation scenario analyzed here, the `.ell` is structurally guaranteed to be unchanged because `MDVault::SaveModule` is skipped when `FUN_10011230` returns false. Only the `.xml` (debugger data) can be rewritten, which is not a runtime linkability risk. (The universal form of this ELL-safety claim is scoped by the TVS-L.8 notice immediately below.)

> **TVS-L.8 SCOPE CORRECTION (2026-08-23):** The universal form of the ELL-safety guarantee
> in this report ("on ANY modify failure") and the claim that `.ell`/`.xml` existence checks
> are sufficient to detect any structural artifact-write failure are **falsified by a
> distinct fault class — post-compile stale-write Fault A** — documented in
> [`RE_L8_STALE_WRITE_FAULT_A.md`](./RE_L8_STALE_WRITE_FAULT_A.md). Fault A occurs when
> `FUN_10011230` returns **true** (compile + codegen succeed) and `MDVault::SaveModule` **is**
> called, but the `.ell` write fails at the OS level and the failure is suppressed
> (`MDWriter::SaveToFile` forces `true` at `0x1001942d` without failbit check; `SaveModule`
> returns `local_15=1`; `CompileText` proceeds to `AddATVerified`/`UpdateProperties`). The
> result is a stale `.ell` + new `.xml` + reported success — passing both existence checks
> while leaving the library runtime-unlinkable. The analysis below remains **correct and
> unchanged for the finalization-truncation scenario it analyzes** (where `FUN_10011230`
> returns **false** and `SaveModule` is **skipped**). Only the *universal* generalization and
> the existence-check-sufficiency claim have been scoped (see the inline notes in §1, §5).

---

## 1. Executive Summary

**CONFIRMED RISK:** orlib **can** truncate/replace a prior known-good `.xml` on a modify operation that fails *after* compile succeeds but *during* finalization (code-generation / symbol-write). The `.ell` (runtime-critical binary) is gated by the finalization return value (`FUN_10011230` must return success for `MDVault::SaveModule` to run) and is therefore *not* truncated in that scenario, producing a **cross-store inconsistency** in debugger data only (new `.xml` written, old `.ell` retained).

**Structural guarantee that DOES hold (compile-stage failure):** A compile-stage failure (syntax/semantic error in `CodeScanner`/`ScanSourceCode`) sets the global error flag `DAT_1023aac8` before any finalization write, so the `.ell`/`.xml` are never touched. This is the case already proven by the L4.2 E2E test.

**Structural guarantee that DOES hold (post-compile finalization failure — `FUN_10011230` returns false):** The `.ell` (runtime-critical binary, written by `MDVault::SaveModule`) is **not** truncated or replaced **in this scenario**. `MDVault::SaveModule` is only called when `FUN_10011230` returns success (`iVar3 == 1`); on failure it is skipped. The `.ell` is therefore structurally guaranteed to be unchanged **when `FUN_10011230` returns false** (compile-stage or post-compile finalization failure). This guarantee is **scoped to the finalization-truncation scenario**; it does **not** cover the distinct post-compile stale-write Fault A class (TVS-L.8), where `FUN_10011230` returns **true**, `SaveModule` **is** called, and the `.ell` write fails at the OS level with the failure suppressed — see [`RE_L8_STALE_WRITE_FAULT_A.md`](./RE_L8_STALE_WRITE_FAULT_A.md).

**What CAN happen on post-compile finalization failure:** The finalization entry `FUN_10011230` calls `SymWriter::Save` (the `.xml` writer) **unconditionally** after `FUN_1008e640` (code generation) and `SymInfo::AdjustOffsets`, without checking `FUN_1008e640`'s return value. `SymWriter::Save` opens the target `.xml` with `STGM_CREATE | STGM_WRITE` (truncate-on-open) via `SHCreateStreamOnFileW`. There is **no temp-file + rename** and **no rollback** of the prior `.xml`. The resulting `.xml` inconsistency is a debugger/source-map issue, not a runtime linkability risk.

**Implication for TVS-L.4 Decision Gate:** The reviewer's option (b) — "produce documented evidence that orlib cannot reach the artifact-write stage on a modify that fails at compile/link, making the invariant structurally guaranteed" — **holds for the finalization-truncation scenario analyzed in this report**: when `FUN_10011230` returns false, the runtime-critical `.ell` is structurally guaranteed to be unchanged (compile-stage or post-compile finalization failure). Only the `.xml` (debugger data, not loaded at runtime) can be rewritten on post-compile finalization failure, which is not a runtime linkability risk and matches TSDev's behavior (TSDev has the same risk and no rollback for `.xml`). **Note (TVS-L.8):** this implication is scoped to the finalization-truncation scenario; the distinct post-compile stale-write Fault A class (where `FUN_10011230` returns true and the `.ell` write fails at the OS level with the failure suppressed) is out of scope for the L.4 option-(b) argument and is documented separately in [`RE_L8_STALE_WRITE_FAULT_A.md`](./RE_L8_STALE_WRITE_FAULT_A.md).

---

## 2. Decompiled Code of `_VerifyModule_New` (relevant portions)

**Address:** `0x10169630` (`_VerifyModule_New@20`), orlib20.dll

```c
uint _VerifyModule_New_20(int param_1, int param_2, int param_3,
                          int param_4, CELCompiler *param_5) {
  ...
  // Phase 1: pre-checks (FUN_10166e70, FUN_10166060). Return 1 on failure.
  iVar2 = FUN_10166e70();
  if ((iVar2 != 0) || (iVar2 = FUN_10166060(), iVar2 != 0)) {
    return 1;                                   // early bail — NO file writes
  }
  ...
  // Phase 2: load source text into hMem (FUN_101087d0)
  if (((local_54 & 2) != 0) ||
      (hMem = FUN_101087d0(param_1, param_2, param_3, local_20, local_18),
       pcVar5 = hMem, hMem != (char *)0x0)) {
    local_1c = pcVar5;
    CELBasicCompiler::CELBasicCompiler(local_1a8);
    ...
    // Phase 3: COMPILE + FINALIZE happen inside CompileText
    CELBasicCompiler::CompileText(local_1a8, local_1c, iVar2, param_3,
                                  pcVar5, param_4, iVar6, ppvVar7,
                                  param_5, pbVar8);
    if (hMem != (char *)0x0) GlobalFree(hMem);
    FUN_101a9a90();
    CELBasicCompiler::~CELBasicCompiler(local_1a8);
    uVar3 = FUN_101698f5();                      // teardown, returns status
    return uVar3;
  }
  ...
}
```

`FUN_101698f5` is pure teardown (resets `DAT_1023aab0`, returns `DAT_1023aac8 == 0`); it performs no file I/O. All compile + finalize work happens inside `CELBasicCompiler::CompileText`.

---

## 3. Finalization Path Call Chain (with addresses)

```
_VerifyModule_New@20 .............. 0x10169630  (orlib20.dll)
  └─ CELBasicCompiler::CompileText  0x1001dd80  (orlib20.dll)
       ├─ CodeScanner .............. 0x1001e8e0  (orlib20.dll) — parse/scan
       │    └─ sets DAT_1023aac8 on failure
       │
       ├─ [if DAT_1023aac8 == 0] FUN_1014de40 / FUN_1014e140 / FUN_1014e5d0
       │   ReWalkInitializationExpressions, UpdateAttributes, ...
       │
       ├─ [if DAT_1023aac8 == 0] FUN_10011230 ........... 0x10011230 (orlib20.dll)  *** FINALIZATION ENTRY ***
       │    ├─ SymInfo::SymInfo, SymInfo::SetVersion, SymInfo::SetFileName
       │    ├─ SymMethod::SymMethod, SymInfo::AddBlock
       │    ├─ FUN_1008e640 ......................... 0x1008e640 (orlib20.dll)  *** CODE GENERATION (in-memory) ***
       │    │    └─ returns (DAT_1023aac8 == 0); can return FALSE on link/codegen error
       │    ├─ SymInfo::AdjustOffsets ........................ (orlib20.dll, vtable)
       │    ├─ SymWriter::SymWriter .......................... (orlib20.dll, vtable)
       │    ├─ SymWriter::Save ............................... 0x100120a0 (ELDebugger.dll)  *** WRITES .xml (XML symbol file) ***
       │    │    └─ XmlDocument::Save -> FormatAndSaveXml ..... 0x100a41b0 (ECL.dll)
       │    │         └─ SHCreateStreamOnFileW(path, 0x1011, &stream)
       │    │              flags = STGM_CREATE | STGM_WRITE | STGM_SHARE_DENY_WRITE
       │    │              *** TRUNCATES target file on open; NO temp+rename ***
       │    ├─ SymWriter::~SymWriter
       │    └─ return uVar5;   // uVar5 = FUN_1008e640 result (NOT checked before SymWriter::Save)
       │
       ├─ [if FUN_10011230 == 1] ELMeta::ModuleInfo::SetGuid
       │   ELMeta::MDVault::SaveModule .................... 0x1001fd80 (ELMDCore.dll)  *** WRITES .ell (binary metadata) ***
       │    └─ MDWriter::SaveToFile ....................... 0x10019320 (ELMDCore.dll)
       │         ├─ SaveToStream (serialize to in-memory ostrstream)
       │         ├─ FUN_1001b3b0 -> FUN_1001c230 -> std::_Fiopen(path, 0x22, 0x40)
       │         │    mode 0x22 = ios::out | ios::binary  *** TRUNCATES on open; NO temp+rename ***
       │         └─ ostream::write(buffer, len)
       │
       └─ [if FUN_10011230 != 1] FUN_10156ad0 (error report) -> goto LAB_1001e2b2
            *** SKIPS MDVault::SaveModule (.ell NOT written) ***
            *** NO rollback of .xml already written by SymWriter::Save ***
```

**Key cross-DLL resolution:** `SymWriter::Save` is imported by orlib20.dll from `ELDebugger.dll` (vtable slot resolved via IAT entry `0x101f9280` → import name `?Save@SymWriter@@QAEXXZ` at `0x10228a98`). `MDVault::SaveModule` is imported from `ELMDCore.dll`. `XmlDocument::Save`/`FormatAndSaveXml` live in `ECL.dll` and are reached via `ELDebugger.dll`'s import of `elsystem::xml::XmlDocument`.

---

## 4. Answers to Q1–Q4 (with evidence)

### Q1: On a modify, if the compile step fails (syntax/semantic error), does `_VerifyModule_New` ever write to `.ell` or `.xml`?

**ANSWER: NO.** The invariant holds for compile-stage failures.

**Evidence** (`CELBasicCompiler::CompileText` @ `0x1001dd80`):

```c
DAT_1023aac8 = CodeScanner(this, param_1, param_2, param_3, param_4,
                           param_5, param_6, param_7, param_8,
                           &local_3c, &local_30, &local_28, local_1a0, &local_34);
if (DAT_1023aac8 == 0) {                         // *** GATE: only if scan succeeded ***
  ... // FUN_1014de40, FUN_1014e140, ReWalkInitializationExpressions, UpdateAttributes
  ...
  if (DAT_1023aac8 == 0) {                       // *** re-check after mid-compile steps ***
    ...
    if (DAT_1023aac8 == 0) {                     // *** re-check before finalization ***
      ShrinkDllInfo(this_01, pIVar8);
      ...
      iVar3 = FUN_10011230(local_1c, ...);       // finalization (writes .xml)
      ...
      if (iVar3 != 1) { goto LAB_1001e2b2; }     // skip MDVault::SaveModule
      ...
      ELMeta::MDVault::SaveModule(...);          // writes .ell
    }
  }
} else {
  // error cleanup — NO file writes, only in-memory record unlinking
}
```

`CodeScanner` (`0x1001e8e0`) sets `DAT_1023aac8` to the result of `ScanSourceCode`/`ScanATSourceCode`. On any non-zero result, `CompileText` takes the `else` branch and never calls `FUN_10011230` (hence never `SymWriter::Save`) nor `MDVault::SaveModule`. The `.ell`/`.xml` are untouched.

This is the case already proven empirically by the L4.2 E2E test (syntax failure leaves old `.ell`/`.xml` intact).

### Q2: On a modify, if compile succeeds but finalization fails (e.g., `SymWriter::Save` throws or `FUN_1008e640` returns false), does `_VerifyModule_New` leave a truncated `.ell`? Or does it roll back?

**ANSWER: It does NOT leave a truncated `.ell`. The `.ell` is unchanged. Only the `.xml` is rewritten. There is NO rollback, but the runtime-critical `.ell` is structurally protected.**

**Evidence** (`FUN_10011230` @ `0x10011230`):

```c
uVar5 = FUN_1008e640(local_1c, param_3, extraout_ECX);   // code gen — returns bool
SymInfo::AdjustOffsets(pSVar1);                           // adjust — no file I/O
SymWriter::SymWriter(local_44, pSVar1);
local_8 = CONCAT31(local_8._1_3_, 6);
SymWriter::Save(local_44);                                // *** WRITES .xml UNCONDITIONALLY ***
SymWriter::~SymWriter(local_44);
...
return uVar5;                                             // returns FUN_1008e640 result
```

`SymWriter::Save` is called **without checking `uVar5`** (the return of `FUN_1008e640`). `FUN_1008e640` returns `(DAT_1023aac8 == 0)` and can return `false` when a codegen/link error sets `DAT_1023aac8` mid-generation (the function checks `DAT_1023aac8` repeatedly and breaks out of its generation loops on error). When that happens:

1. `SymWriter::Save` still executes and overwrites/truncates the target `.xml` (debugger/source-map data).
2. `FUN_10011230` returns `false` (uVar5).
3. Back in `CompileText`, `iVar3 != 1` → `goto LAB_1001e2b2`, **skipping** `MDVault::SaveModule`. The `.ell` (runtime-critical binary) is **NOT** written — the prior `.ell` is retained unchanged.

**Result:** old `.ell` (unchanged, runtime-critical) + new/truncated `.xml` (debugger data) → **cross-store inconsistency in debugger data only**. In this finalization-truncation scenario (`FUN_10011230` returns false), the `.ell` (what Desktop loads for runtime linking) is not truncated because the write is never attempted.

> **TVS-L.8 scope note:** The "NEVER truncated on any modify failure" wording that appeared
> here is scoped to the finalization-truncation scenario. It does **not** cover the
> post-compile stale-write Fault A class (TVS-L.8), where `FUN_10011230` returns **true**,
> `SaveModule` **is** called, and the `.ell` write fails at the OS level with the failure
> suppressed — see [`RE_L8_STALE_WRITE_FAULT_A.md`](./RE_L8_STALE_WRITE_FAULT_A.md).

The error path (`FUN_10156ad0` @ `0x10156ad0`) only records error info into globals (`DAT_1023acec`, `DAT_1023acf0`, `DAT_1023aab4`) and increments `DAT_1023aac4`. It performs **no file restoration** — no `CopyFile` of a backup, no delete of the partial `.xml`, no restore of the prior `.xml`. No restoration is needed for the `.ell` because it was never written.

### Q3: Does `SymWriter::Save` or `MDVault::SaveModule` write atomically (temp + rename) or directly?

**ANSWER: Both write DIRECTLY (truncate-on-open). Neither uses temp+rename.**

**Evidence — `SymWriter::Save` path** (`ELDebugger.dll` `0x100120a0` → `ECL.dll` `FormatAndSaveXml` `0x100a41b0`):

```c
// ECL.dll, FormatAndSaveXml:
SHCreateStreamOnFileW((LPCWSTR)param_3.lpVtbl, 0x1011, &local_18);
// 0x1011 = STGM_CREATE (0x1000) | STGM_WRITE (0x1) | STGM_SHARE_DENY_WRITE (0x10)
// STGM_CREATE: "if the file exists, it is truncated to zero length before write"
```

No temporary path is constructed; the stream is opened directly on the target filename (`%WorkDir%\Bin\<name>.xml`, built in `SymWriter::Save` as `"%s\\Bin\\%s"`). The "FormatAndSaveXml" function name confirms this path writes the XML symbol file (`.xml`), not the `.ell` binary.

**Evidence — `MDVault::SaveModule` path** (`ELMDCore.dll` `0x1001fd80` → `MDWriter::SaveToFile` `0x10019320`):

```c
// ELMDCore.dll, MDWriter::SaveToFile:
bVar3 = SaveToStream(this, (ostrstream *)local_c0);   // serialize to in-memory stream
if (bVar3) {
  FUN_1001b3b0(local_180, (wchar_t *)param_1);         // open fstream on target path
  //  -> FUN_1001c230 -> std::_Fiopen(path, 0x22, 0x40)
  //     mode 0x22 = ios::out (0x20) | ios::binary (0x2)  -> TRUNCATE on open
  std::basic_ostream::write(local_170, (char *)*local_a0, len);
}
```

The serialization to an in-memory `ostrstream` happens *before* the file open, so the bytes are fully prepared — but the file is opened in truncate mode directly on the final path. If the `ostream::write` fails partway (disk full, I/O error), the file is left truncated/partial. There is no `rename(temp, final)` step.

### Q4: Is there any evidence that orlib handles finalization failure by restoring the prior `.ell` state?

**ANSWER: NO.** There is no restore/rollback of individual `.ell`/`.xml` artifacts anywhere in the `_VerifyModule_New` → `CompileText` → `FUN_10011230` path.

**Evidence — searched for rollback mechanisms:**

| Search | Result |
|--------|--------|
| `search_strings(".bak")` | 5 hits — all `*MASTER.BAK` (SMASTER/FMASTER/IMASTER/MMASTER/BMASTER). These are backups of the **warehouse master files** (`*.MST`), not of individual `.ell`/`.xml` artifacts. |
| `search_strings("backup")` | 4 hits — `BackUpMasterFiles`, `BackUpMasterRec`, `GetBackUpRec`, `SetBackUpRec`. All operate on master records, not `.ell`/`.xml`. |
| `search_strings(".tmp")` | 1 hit — `compress.tmp` (unrelated). |
| `get_xrefs_to(BackUpMasterFiles @ 0x1011f380)` | Only a DATA ref from `0x1021d530` (a function-pointer table with no inbound xrefs) + the export entry point. **Not called from `_VerifyModule_New` or `CompileText`.** |

`BackUpMasterFiles` (`0x1011f380`) copies `*MASTER.MST` → `*MASTER.BAK` via `_OR_CopyFile_8` *only when the master file size matches an expected record count* — it is a warehouse-level integrity backup, invoked from a separate code path, not from the per-module verify/finalize path. It does **not** back up `.ell` or `.xml` files.

The `FUN_10011230` unwind table (local_8 state indices 0..6) only destroys the `SymWriter` and `SymInfo` temporaries on exception; it does **not** restore any prior `.ell` file.

---

## 5. Conclusion

### Can orlib truncate/replace a prior known-good ELL/XML on a failed modify?

> **TVS-L.8 scope note:** The conclusion below covers the **finalization-truncation** scenario
> only (where `FUN_10011230` returns **false** and `MDVault::SaveModule` is **skipped**). It
> does **not** cover the distinct post-compile stale-write **Fault A** class (where
> `FUN_10011230` returns **true**, `SaveModule` **is** called, and the `.ell` write fails at
> the OS level with the failure suppressed), which is documented in
> [`RE_L8_STALE_WRITE_FAULT_A.md`](./RE_L8_STALE_WRITE_FAULT_A.md). The universal wording
> ("NEVER ... on ANY modify failure") that appeared in earlier versions of this section is
> scoped here to the finalization-truncation scenario.

**In the finalization-truncation scenario, the `.ell` (runtime-critical binary) is not truncated/replaced.** Only the `.xml` (debugger/source-map data) can be rewritten, and only under one specific condition:

> **A modify operation in which the compile/scan stage succeeds (`DAT_1023aac8 == 0` after `CodeScanner`) but the finalization code-generation stage (`FUN_1008e640`) fails partway (sets `DAT_1023aac8 != 0` during codegen).**

In that case:
- `FUN_10011230` calls `SymWriter::Save` **unconditionally** after `FUN_1008e640` returns, overwriting/truncating the target `.xml` (XML symbol file) at `%WorkDir%\Bin\<name>.xml`.
- `FUN_10011230` then returns `false`, so `CompileText` skips `MDVault::SaveModule`.
- The `.ell` (runtime-critical binary) is **NOT** written → the old `.ell` remains unchanged.
- **Net result:** old `.ell` (unchanged, runtime-critical) + new/truncated `.xml` (debugger data) → cross-store inconsistency in debugger data only. No rollback, but no runtime linkability risk.

**Failure-mode summary:**
1. **Compile failure** (syntax/semantic error in `CodeScanner`/`ScanSourceCode`): `DAT_1023aac8` is set before `FUN_10011230` is ever called. `.ell`/`.xml` both untouched. *(Structurally guaranteed — proven by the L4.2 E2E test.)*
2. **Post-compile finalization failure** (`FUN_1008e640` fails after `CodeScanner` succeeds): `SymWriter::Save` runs unconditionally → `.xml` rewritten/truncated; `MDVault::SaveModule` skipped → `.ell` NOT written (old `.ell` retained). `.ell` unchanged, `.xml` may be inconsistent.
3. **Pre-compile bail** (`FUN_10166e70`/`FUN_10166060` return non-zero): returns 1 before any file I/O. `.ell`/`.xml` untouched.
4. **Successful modify**: both `.ell` and `.xml` written (consistent replacement).
5. **Post-compile stale-write Fault A** (TVS-L.8; **not covered by this report's guarantee**): compile + codegen succeed (`FUN_10011230` returns `1`), `MDVault::SaveModule` **is** called, but the `.ell` write fails at the OS level (e.g. read-only target → `CreateFileW` `INVALID_HANDLE`) and the failure is suppressed (`MDWriter::SaveToFile` forces `true` at `0x1001942d` without failbit check; `SaveModule` returns `local_15=1`; `CompileText` proceeds to `AddATVerified`/`UpdateProperties`). Result: stale `.ell` + new `.xml` (new GUID absent from `.ell`) + reported success. Documented in [`RE_L8_STALE_WRITE_FAULT_A.md`](./RE_L8_STALE_WRITE_FAULT_A.md).

### Is the invariant "old .ell intact on any modify failure" structurally guaranteed?

**For the finalization-truncation scenario analyzed in this report (modes 1–3 above): YES.** The `.ell` is written only by `MDVault::SaveModule`, which is gated on `FUN_10011230` returning success (`iVar3 == 1`). When `FUN_10011230` returns false (compile-stage or post-compile finalization failure), `MDVault::SaveModule` is skipped, so the prior `.ell` is retained unchanged.

**This invariant does NOT hold as a universal claim over all modify-failure classes.** Fault A (mode 5 above, TVS-L.8) is a distinct class where `FUN_10011230` returns **true** and `SaveModule` **is** called; the `.ell` write is **attempted** and fails at the OS level, with the failure suppressed so the operation reports success with a stale `.ell`. The earlier universal wording ("on ALL modify failure modes") is therefore scoped to the finalization-truncation scenario. See [`RE_L8_STALE_WRITE_FAULT_A.md`](./RE_L8_STALE_WRITE_FAULT_A.md).

The `.xml` (debugger/source-map data) is NOT structurally guaranteed on post-compile finalization failure, because `SymWriter::Save` runs unconditionally. However, the `.xml` is not loaded at runtime by Desktop for linking — it is debugger metadata. The `.xml` inconsistency is therefore a debugger/source-map issue, not a runtime linkability risk.

### TSDev parity

The reviewer's option (b) ("documented evidence that orlib cannot reach the artifact-write stage on a modify that fails at compile/link") **holds for the finalization-truncation scenario analyzed in this report**: when `FUN_10011230` returns false, the runtime-critical `.ell` is structurally guaranteed to be unchanged (compile or post-compile finalization failure).

> **TVS-L.8 note:** This parity argument is scoped to the finalization-truncation scenario. It
> does **not** extend to the post-compile stale-write Fault A class (TVS-L.8), where
> `FUN_10011230` returns true and the `.ell` write fails at the OS level with the failure
> suppressed. The L.8 decision-gate policy for Fault A is **BLOCKED** — see
> [`RE_L8_STALE_WRITE_FAULT_A.md`](./RE_L8_STALE_WRITE_FAULT_A.md) §7.

TSDev has the same behavior: it does not roll back the `.xml` on post-compile finalization failure. The `.xml` inconsistency is a known, accepted condition in TSDev itself. Replicating TSDev's behavior (no rollback for `.xml`) is the correct parity target — adding a rollback that TSDev does not perform would diverge from TSDev and risk desynchronizing the BST (which references the new source) from the `.xml`.

### No backup/restore needed (finalization-truncation scenario)

> **TVS-L.8 scope note:** The "no backup/restore needed" conclusion below applies to the
> **finalization-truncation scenario** only. It does **not** extend to the post-compile
> stale-write Fault A class (TVS-L.8), for which no recovery mechanism is established and the
> decision gate is BLOCKED. See [`RE_L8_STALE_WRITE_FAULT_A.md`](./RE_L8_STALE_WRITE_FAULT_A.md).

No backup/restore is needed for the finalization-truncation modify path, because:

1. **In the finalization-truncation scenario, the `.ell` (runtime-critical artifact) is structurally guaranteed to be unchanged when `FUN_10011230` returns false.** `MDVault::SaveModule` is skipped. There is no path by which a finalization-truncation modify failure can truncate or replace the prior `.ell`.
2. **Restoring the old `.xml` would create a different inconsistency** — the BST already references the new source text (via `saveSource`), so restoring the old `.xml` would desynchronize the BST from the `.xml`. TSDev does not do this, and neither should tsdev_automation.
3. **The plan prohibits direct byte backup/restore unless TSDev uses an equivalent sequence.** TSDev does not perform `.xml` rollback on modify failure, so tsdev_automation must not either.

> **TVS-L.8 correction (removed claim):** The earlier statement that "the existing production
> postconditions (`.ell` and `.xml` existence checks in `doInsert`/`doModify`) are sufficient
> to detect any structural artifact-write failure that would compromise runtime linkability"
> is **removed**. It is falsified by Fault A (TVS-L.8): a stale `.ell` (exists, unchanged) +
> new `.xml` (exists, new GUID) + reported success passes both existence checks while leaving
> the library runtime-unlinkable. Existence checks are necessary but **not sufficient** to
> detect the Fault A class. See [`RE_L8_STALE_WRITE_FAULT_A.md`](./RE_L8_STALE_WRITE_FAULT_A.md).

### Recommended hardening for tsdev_automation modify path

For the **finalization-truncation scenario**, no backup/restore is needed: the `.ell` is structurally guaranteed to be unchanged when `FUN_10011230` returns false, and the `.xml` inconsistency matches TSDev's behavior. The `.ell`/`.xml` existence postconditions remain **necessary** checks.

> **TVS-L.8:** Existence checks are **not sufficient** for the post-compile stale-write Fault
> A class, which passes both existence checks while reporting success with a stale `.ell`.
> Fault A recovery is BLOCKED pending a TSDev-equivalent recovery mechanism — see
> [`RE_L8_STALE_WRITE_FAULT_A.md`](./RE_L8_STALE_WRITE_FAULT_A.md) §7. No production
> workaround is authorized by this report.

---

## 6. Function Address Index

| Function | Address | DLL | Role |
|----------|---------|-----|------|
| `_VerifyModule_New@20` | `0x10169630` | orlib20.dll | Exported verify entry |
| `CELBasicCompiler::CompileText` | `0x1001dd80` | orlib20.dll | Compile + finalize orchestrator |
| `CodeScanner` | `0x1001e8e0` | orlib20.dll | Parse/scan; sets `DAT_1023aac8` |
| `FUN_10011230` | `0x10011230` | orlib20.dll | **Finalization entry** (SymInfo + SymWriter) |
| `FUN_1008e640` | `0x1008e640` | orlib20.dll | Code generation (in-memory); returns `(DAT_1023aac8==0)` |
| `SymInfo::AdjustOffsets` | vtable | orlib20.dll | Offset fixup (no file I/O) |
| `SymWriter::Save` | `0x100120a0` | ELDebugger.dll | **Writes `.xml`** (XML symbol file) |
| `XmlDocument::Save` | `0x100a3e30` | ECL.dll | XmlDocument save entry |
| `FormatAndSaveXml` | `0x100a41b0` | ECL.dll | `SHCreateStreamOnFileW(path, 0x1011, ...)` — truncate-on-open |
| `MDVault::SaveModule` | `0x1001fd80` | ELMDCore.dll | **Writes `.ell`** (binary metadata) |
| `MDWriter::SaveToFile` | `0x10019320` | ELMDCore.dll | `std::_Fiopen(path, 0x22, 0x40)` — truncate-on-open |
| `FUN_101698f5` | `0x101698f5` | orlib20.dll | Teardown (no file I/O) |
| `FUN_10156ad0` | `0x10156ad0` | orlib20.dll | Error report (no rollback) |
| `BackUpMasterFiles` | `0x1011f380` | orlib20.dll | `*.MST`→`*.BAK` (warehouse master; NOT in verify path) |
| `DAT_1023aac8` | `0x1023aac8` | orlib20.dll | Global error flag (0 = ok) |

---

## 7. Methodology / Reproducibility

- Ghidra project `TSDevAnalysis`, instance PID 37816, TCP `127.0.0.1:8089`.
- Programs analyzed: `/orlib20.dll` (image base `0x10000000`), `/receivers/ELMDCore.dll`, `/receivers/ELDebugger.dll`, `/receivers/ECL.dll` (imported on-demand for this analysis).
- Decompile + disassemble + read_memory + get_xrefs_to + search_strings + search_functions + list_external_locations.
- Cross-DLL vtable resolution: `FUN_10011230` disassembly showed `CALL dword ptr [0x101f9280]` (SymWriter::Save IAT slot); `read_memory(0x101f9280)` → `0x10228a98`; `read_memory(0x10228a98)` → import name string `?Save@SymWriter@@QAEXXZ`; located export in `ELDebugger.dll` via `search_strings("SymWriter")` → `Save @ 0x100120a0`.
- `FormatAndSaveXml` flag decode: `0x1011 = 0x1000 | 0x10 | 0x1` = `STGM_CREATE | STGM_SHARE_DENY_WRITE | STGM_WRITE` (per `objbase.h`).
- `MDWriter::SaveToFile` mode decode: `0x22 = 0x20 | 0x2` = `ios::out | ios::binary` (truncate-on-open per C++ stdlib semantics).
