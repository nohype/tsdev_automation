# TSDev Automation — Reverse-Engineering Feasibility Study

**Status:** Synthesis report (read-only RE phase complete)
**Date:** 2026-08-15
**Scope:** Determine how a standalone C++ CLI can implement three TSDev Library operations (insert / delete / modify, each followed by compile/verify) without using the TSDev.exe GUI, with least custom implementation and least complexity.

> **Current status:** Workstream L is closed because its team completed it and a
> final independent review was completed.
>
> The L chronology and verification claims in this dated synthesis report are
> retained as historical provenance. Current CLI/library procedures have no
> general TradeStation Desktop/TSDev running-state prerequisite; a procedure
> that explicitly exercises Desktop state documents that state in its own
> procedure. Current Workstream M status is: M.1/M.2/M.3 are approved historical
> slices; M.3b is planned; M.4 implementation exists but is blocked by the
> CLI-created object-code defect; M.5 is pending. No approved post-TVS
> production-E2E claim exists for M.4.

> **HISTORICAL SUPERSESSION BANNER — 2026-08-23 (Workstream L root-cause confirmation)**
>
> Confirmed Unknown-link findings have invalidated several core claims in this
> document. **Historical record:** Production runtime acceptance was recorded as **BROKEN.** Strategies
> inserted/modified/upserted by the production `tsdev_cli` appear VERIFIED in
> TSDev but fail at runtime in TradeStation Desktop with "Unknown link error
> occurred." A full Desktop restart does not repair a broken CLI Strategy, so
> the failure is durable, not a live-cache artifact. Other supported component
> types share the compile/finalization path and must be validated by Workstream
> L, but equivalent runtime failure is not claimed without type-specific
> evidence. The library-level "IMPLEMENTED and APPROVED" statements below
> remain accurate *only* for the warehouse/compile-persistence layer; they do
> **not** constitute runtime-link acceptance.
>
> **Historical status at the time of this 2026-08-23 record:** Workstream L
> (Runtime-Link Root-Cause Fix) had not yet been closed. The approved plan was
> `docs/workstream_unknown_link_fix_plan.md`. TVS-L.1 (RED
> harness), TVS-L.2 (STA COM lifecycle guard), and TVS-L.3 (the `FUN_10011230`
> persistence bypass is removed; the real finalization path now runs) are
> COMPLETE. The TVS-L.4/L.5 completion claim was **FALSIFIED by persistence
> and deletion fault injection** and is **superseded by the L.6–L.15
> recovery**, which is **implemented on branch `workstream-L-recovery`** (L.8
> BLOCKED — no invented TSDev recovery; L.9 detect-and-fail stale-ELL
> modify/upsert; L.12 detect-and-report orphan-ELL delete; existence checks
> are necessary but not sufficient). **No new Desktop runtime re-gate is
> claimed without an L.17 result.** At the time of this historical record,
> Workstream M remained BLOCKED pending L.17. Current M status is at the top of
> this document. Durable static evidence:
> `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`. Approved research plan:
> `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`. Dynamic A/B proof artifacts:
> `tmp/com_unpatched_probe/`, `tmp/com_nopatch_6689.ell`,
> `tmp/com_nopatch_6689.xml`.
>
> **Claims in this document now SUPERSEDED** (corrections applied in-line at
> each affected section; historical text preserved with correction rather than
> destructive rewriting):
> 1. The "partially initialized CLR" explanation for the `FUN_10011230` patch
>    (§5.6 root cause) is **wrong/unproven**. The real prerequisite is
>    **per-thread STA COM** for the MSXML wrapper used by `SymWriter::Save`.
> 2. The "XML not written is acceptable" claim (§5.6 consequence) is
>    **superseded**. XML itself is debugger/source-map data and is not consumed
>    by the runtime linker, but its **absence is a visible marker of the
>    bypass**; the causal defect is the bypassed original
>    compiler/link-finalization path (`FUN_1008E640` → `SymInfo::AdjustOffsets`
>    → `SymWriter::Save`), which produces runtime-unlinkable output.
> 3. The patch-as-fix narrative (§5.6 fix) and the ABI justification
>    "`__fastcall` with 5 parameters ... hence `RET 0x0C`" are **superseded**.
>    Durable Ghidra evidence (`docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`) shows the
>    original callee ends in **plain `RET`** (`0x10011454`) and the caller
>    executes **`ADD ESP,0x0C`** (`0x1001E0C1`): the original ABI is
>    fastcall-like register passing with **caller cleanup** for the 12 stack
>    bytes. The patch's `RET 0x0C` changes that cleanup contract — a static ABI
>    hazard, not a correct ABI match.
>
> **HISTORICAL TVS-L.3 UPDATE — 2026-08-23 (persistence bypass removed):** The
> `FUN_10011230` persistence patch has been **completely removed** from
> production code in TVS-L.3. The real finalization path
> (`FUN_10011230` → `FUN_1008E640` → `SymInfo::AdjustOffsets` →
> `SymWriter::Save`, then `ModuleInfo::SetGuid` / `MDVault::SaveModule`) now
> runs end-to-end under the per-thread STA COM prerequisite established in
> TVS-L.2. The earlier "still present ... slated for complete removal ... No
> implementation has been performed" statement below is **superseded** by this
> completion note (historical text preserved rather than destructively
> rewritten). The historical post-L.3/pre-M.1 catalog phase and the current
> tracked totals (signature definitions, tracked exports, data globals, and
> patch sites) — including the former `FUN_10011230_persistence_patch` entry
> and its patch site being gone — are documented in the SSOT
> `docs/SIGNATURE_CATALOG.md` (current and historical resolver/catalog counts).
>
> The historical text immediately below (preserved) read: "The patch is still
> present in production code and is slated for **complete removal** in TVS-L.3
> (after TVS-L.2 establishes the STA COM prerequisite that makes the unpatched
> path safe to run). No implementation has been performed."

---

## 1. Executive Summary

> **HISTORICAL EXECUTIVE STATUS CORRECTION — 2026-08-23 (updated TVS-L.3 — patch
> removed; this dated record predates Workstream L closure):** The "All three core functions
> (INSERT, DELETE, MODIFY) are now IMPLEMENTED and APPROVED — the primary
> acceptance criteria ... is satisfied at the library level" statement below
> is **accurate only at the library/warehouse-persistence layer**. It is
> **NOT** a runtime-link acceptance statement. Confirmed Unknown-link
> findings showed that CLI-produced strategies failed in TradeStation Desktop
> with "Unknown link error occurred" despite TSDev reporting them VERIFIED.
> **Production runtime acceptance was BROKEN** while the `FUN_10011230`
> persistence patch was present. At that time, Workstream L had not yet been closed. The approved plan was
> `docs/workstream_unknown_link_fix_plan.md`;
> TVS-L.1 (RED harness), TVS-L.2 (STA COM lifecycle guard), and TVS-L.3 (the
> `FUN_10011230` persistence bypass is removed; the real finalization path
> now runs under the per-thread STA COM prerequisite) are COMPLETE. The
> TVS-L.4/L.5 completion claim was **FALSIFIED by persistence and deletion
> fault injection** and is **superseded by the L.6–L.15 recovery**, which is
> **implemented on branch `workstream-L-recovery`** (L.8 BLOCKED — no
> invented TSDev recovery; L.9 detect-and-fail stale-ELL modify/upsert; L.12
> detect-and-report orphan-ELL delete; existence checks are necessary but
> not sufficient). The historical record did not claim a new Desktop
> runtime re-gate without an L.17 result. Current Workstream M status is
> recorded at the top of this document; M.4 remains blocked by the CLI-created
> object-code defect. The `FUN_10011230` patch referenced below was the bypass
> that caused the runtime-unlinkable output; it is no longer present in
> production code. See the top-of-file supersession banner and §5.6 for the
> corrected root cause.
>
> **Historical narrative note:** The executive prose below retains the feasibility
> study's pre-L/M implementation narrative; it is not a current acceptance or
> workstream-status claim.

We reverse-engineered the TradeStation Development Environment (TSDev 9.5) to determine how a standalone CLI can insert, delete, and modify EasyLanguage strategies/functions in the TSDev Library and compile/verify them, without the TSDev GUI. Two viable approaches were identified. **Approach 1** drives `ELIS.exe`, the registered COM LocalServer that wraps the library engine (`IELServices` dispatch interface, 7 methods). **Approach 2** loads `orlib20.dll` directly and calls its exported C functions, which are the same primitives TSDev.exe itself uses. Approach 1 is cleaner to call but has structural gaps (no name-to-ProgId resolution, XML-file import rather than raw source, void error reporting on delete/verify, and a likely TradeStation Desktop coupling). Approach 2 exposes every primitive needed (lookup, create, set-source, compile/verify, error retrieval, save, delete). Parameter orders for the key primitives were recorded as **CONFIRMED** in this
historical study. The standalone INSERT persistence observation was
**historical/pre-L.3 and patch-based** (compile + `.ell` file write via an
in-process patch to `FUN_10011230`; see historical §5.6), not current runtime
acceptance. **Historical DELETE implementation record: IMPLEMENTED and APPROVED** — the orchestrator-based approach calling `FUN_101a8360` (RVA `0x1A8360`) persists deletions to `MWRKArea.wrk` standalone, verified by two independent test runs (see §5.3 DELETE and §5.7). **Historical MODIFY implementation record: IMPLEMENTED and APPROVED** — it composes only existing INSERT primitives (`findExistPos` → `saveSource` → `verifyModule`) with no new orlib20 calls and no new RVAs; verified by two independent test runs (exit 0, ProgId preserved, `.ell` timestamp updated, compile errors surfaced, negative + compile-error tests return `success=false`; see §5.3 MODIFY and §5.8). The headless `_OrlibInit` smoke test has **PASSED**. **Historical implementation record: all three core functions (INSERT, DELETE, MODIFY) were IMPLEMENTED and APPROVED** — the primary acceptance criteria ("the three functions must work reliably and consistently via a CLI") is satisfied at the library level. **Recommendation:** adopt Approach 2 (orlib20.dll direct) as the primary path because it is the only approach that can satisfy all three acceptance criteria headless; retain Approach 1 as a fallback for the subset of operations it supports. Remaining unknowns are narrow: three warehouse-type mappings remain unresolved (ActivityBar/PaintBar/ProbabilityMap), while five component types are confirmed (authoritative type→whType mapping in `docs/TS_COMPONENT_TYPES.md`); minor unmapped fields (see §5.5).

---

## 2. Goal & Acceptance Criteria

The project must provide a C++ CLI that performs three operations against the TSDev Library (`C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin`), reliably and consistently, without the TSDev.exe GUI:

1. **Insert** — Given EasyLanguage source code and a name, insert a new strategy/function into the library and compile/verify it. **Fail loudly** if compilation fails.
2. **Delete** — Given the name of an existing strategy/function, delete it from the library. **Notify** if it does not exist.
3. **Modify** — Given the name of an existing strategy/function and new EasyLanguage source, replace its code and recompile/re-verify. **Notify** if it does not exist; **fail loudly** if compilation of the new code fails.

All three must work via a CLI built with the Visual Studio Build Tools. **Normal CLI operations keep the TradeStation installation read-only and never modify it; the opt-in L7 fault-injection path is the scoped exception described in §7.**

---

## 3. System Under Study

| Component | Path | Role |
|-----------|------|------|
| TSDev.exe | `Program\TSDev.exe` | 32-bit PE32, 2,178,032 bytes. The GUI host that orchestrates library operations. READ-ONLY. |
| TSDev Library | `MyWork\Bin` | **Historical/transient inventory snapshot (report date 2026-08-15; not a current filesystem count):** 3,413 `.ell` (compiled/encrypted, magic `RNAM`) + 6,871 `.xml` (symbol/debug metadata, root `<symbols file=... version={GUID}>`). Files named `elmodule_<X>_<N>.ell/.xml` and `tss<hex>.xml`. X in {0,1,2,3,4,1026,1282,4610,5122}. No index/catalog file. The cited `evidence_A_filesystem_survey.md` and `evidence_A2_binary_static_survey.md` survey files are not checked in and are unavailable in the current tracked repository; no current inventory is implied. |
| orlib20.dll | `Program\orlib20.dll` | 32-bit PE32, image base `0x10000000`, 699 exports. The library engine: init/teardown, name lookup, create entry, set source, compile/verify, error retrieval, save, delete, path building. |
| ELRte.dll | `Program\ELRte.dll` | EL runtime: `CreateRte`/`DestroyRte`/`GetVault`. |
| ELMDCore.dll | `Program\ELMDCore.dll` | Metadata vault: `MDVault::SaveModule`, `RemoveModule`, `RemoveChainedModules`. |
| ECL.dll | `Program\ECL.dll` | Exception creation. |
| tselc.dll | `Program\tselc.dll` | EL compiler (invoked indirectly via verify). |
| ELIS.exe | `Program\ELIS.exe` | 32-bit ATL/C++ COM LocalServer wrapping orlib20. ProgID `ELIsolatedServices.ELServices` (.1). |
| TSCLUtil.exe / TSELAppHost.exe | `Program\` | Unrelated to library ops (BITS tray app / GUI host). Not usable as a CLI. |

orlib20.dll dependencies (all in the TS Program directory): `TsShare.dll`, `tslib.dll`, `orshr20.dll`, `ELRte.dll`, `ELMDCore.dll`, `ECL.dll`, `tskit.dll`, `tsdmgr.dll`, `mfc140u.dll`, `KERNEL32.dll`, `ADVAPI32.dll`. It does **not** import from `TradeStation.exe` or `TSDev.exe`.

---

## 4. Approach 1: ELIS.exe COM Automation

### 4.1 Interface facts

- **Server:** `ELIS.exe`, 32-bit ATL/C++ COM LocalServer. STA (`CComSingleThreadModel`).
- **ProgID:** `ELIsolatedServices.ELServices` (also `.1`), registered in the 64-bit hive.
- **CLSID:** `{44D304D4-55F4-455C-B159-FB88DD525B47}` (CLSID/LocalServer32 in WOW6432Node).
- **AppID:** `{03118EB4-E0C4-4F93-B44C-21C4C65FA1A8}` — `RunAs = Launching User`, no surrogate.
- **TypeLib:** `{9B55A35C-FEC7-4D9B-9947-5D13ECAD0C31}`.
- **IELServices:** IDispatch, IID `{5F66904B-7CDF-4687-9225-834680F025B1}`. 7 custom stdcall methods.
- **_IELServicesEvents:** EMPTY (no events). `IELSecureServices` typelib is NOT embedded/registered (inaccessible).
- ELIS.exe imports from orlib20.dll: `InstallVerifyAllEx`, `_SetStrategyAdopterMode@4`, `_OrlibInit@8`, `_OrlibExit@0`.

### 4.2 Method table

> **Historical/transient evidence (not checked in; unavailable in the tracked
> checkout):** The method signatures, dispids, and parameter types below are from
> the `LoadTypeLibEx` type-library dump in `evidence_C1_elis_typelib.md` §2.5
> (IELServices TypeInfo [2]). The COM registration facts (ProgID/CLSID/AppID/
> TypeLib) are in `evidence_C1_elis_typelib.md` §1; the ELIS.exe string-extracted
> ProgID/CLSID/method-name strings are corroborated in
> `evidence_A2_binary_static_survey.md` §5 (ELIS.exe).

| memid | Method | Signature | Notes |
|-------|--------|-----------|-------|
| 1 | `ImportSourceCode` | `(filePath:BSTR, toPath:BSTR, documentElement:BSTR, itemElement:BSTR, results:BSTR*[out]) -> BOOL` | Imports from an XML file, not raw source. |
| 2 | `VerifyAll` | `(buildNumber:BSTR) -> void` | Verifies entire library; no per-item targeting. |
| 3 | `ExtendedProperties` | get: `(WHType:int, ProgId:int) -> BSTR`; put: `(WHType:int, ProgId:int, BSTR) -> void` | Requires ProgId already known. |
| 4 | `SynchronizeStrategyNetwork` | `(items:BSTR, missingItems:BSTR*[out], xpathList:BSTR, WHType:BSTR, itemName:BSTR) -> int` | Strategy-network sync. |
| 5 | `DeleteAnalysisTechnique` | `(WHType:int, ProgId:int) -> void` | Requires ProgId; void return (no error reporting). |
| 6 | `ImportBinaries` | `(filePath, toPath, documentElement, itemElement, results*) -> BOOL` | Binary import from file. |

### 4.3 Mapping to the three functions

| Function | Approach 1 coverage | Gap |
|----------|---------------------|-----|
| Insert | `ImportSourceCode` (memid 1) — but takes an XML file path, not raw EasyLanguage source. `VerifyAll` (memid 2) compiles the whole library, not the new item. | No raw-source insert; no targeted verify; no compile-error retrieval. |
| Delete | `DeleteAnalysisTechnique` (memid 5) — takes `(WHType, ProgId)`. | No name-to-ProgId lookup exposed; void return gives no not-found/error signal. |
| Modify | No direct method. Would require delete + insert, both with the gaps above. | No modify primitive. |

### 4.4 Gaps and verdict

- **No name-to-ProgId resolution.** Every method that targets an item requires `(WHType, ProgId)`. The interface exposes no lookup. A caller would have to resolve names out-of-band (e.g., scan `MyWork\Bin` XML metadata), duplicating engine logic.
- **XML-file import, not raw source.** `ImportSourceCode` expects a structured XML document, not a plain EasyLanguage text blob. The CLI would have to synthesize the XML wrapper format (undocumented).
- **Void error reporting on delete/verify.** `DeleteAnalysisTechnique` returns void; `VerifyAll` returns void. There is no way to detect "not found" or compile failure through the interface. `ImportSourceCode` returns BOOL + a results BSTR, which is the only error channel.
- **RunAs = Launching User.** Activation runs as the invoking user; acceptable for a local CLI but means no service account isolation.
- **32-bit only.** The CLI must be 32-bit (or use WOW64/surrogate) to call this in-proc-equivalent LocalServer.
- **Possible TradeStation Desktop dependency.** ELIS.exe is a TradeStation component; `VerifyAll` and the verify path may assume the Desktop/runtime is available. Not confirmed headless.
- **`IELSecureServices` inaccessible.** The secure services typelib is not embedded/registered, so any privileged operations are unavailable.

**Verdict:** Approach 1 is **not suitable as the primary approach**. It cannot satisfy the acceptance criteria without substantial out-of-band work (name resolution, XML synthesis, error inference) and lacks a modify primitive and reliable error reporting. It is viable only as a **fallback** for delete (if name resolution is handled externally) and possibly bulk verify.

---

## 5. Approach 2: orlib20.dll Direct Invocation

All findings below are from the historical static PE analysis (pefile + capstone) of `orlib20.dll`; the initial findings are retained as historical research. The raw evidence files named by the original study (`evidence_C2a_orlib20_lifecycle.md`, `evidence_C2b_orlib20_build.md`, `re_report.md`, and the disassembly files) were under gitignored `tmp\re_artifacts\`; they are not checked in and are unavailable in the current tracked checkout. The current durable references are `docs/SIGNATURE_CATALOG.md`, `docs/TS_COMPONENT_TYPES.md`, and the tracked `docs/RE/` findings. **Historical RE method note:** Live RE was initially blocked by a Frida helper-path bug; a later Python Frida MCP workflow was recorded, but that workflow is superseded and is not current guidance. Current runtime RE follows `AGENTS.md`: use Frida directly from the CLI and the working `ghidra` MCP server for static analysis. The historical evidence is cited by section and function where possible; no replacement raw artifact is implied.

### 5.1 Primitive table

| Primitive | Signature | Confidence | Evidence |
|-----------|-----------|------------|----------|
| Init | `BOOL __stdcall _OrlibInit@8(const char* workDirPath, int contextId)` | CONFIRMED | `evidence_C2a_orlib20_lifecycle.md` §2.1; re_report 2.1; `re_disasm.txt` L5-139; `orlib_param_orders.md` §2. Ref-counted at `0x1023acb0`. **Confirmed call:** `OrlibInit(NULL, 1)` → TRUE. `workDirPath` is a **NULL sentinel** (orlib20 derives `MyWork` internally; NOT a path string, NOT an HInstance). `contextId = 1`, passed to `ELRte::CreateRte`. |
| Teardown | `BOOL __stdcall _OrlibExit@0(void)` | HIGH | `evidence_C2a_orlib20_lifecycle.md` §2.2; re_report 2.2; `re_disasm.txt` L227-275. Decrements refcount; tears down at zero via `ELRte::DestroyRte`. |
| Name->ID lookup | `int __stdcall FindExistPos(const char* name, int whType) -> int` | CONFIRMED | `evidence_C2a_orlib20_lifecycle.md` §2.4; re_report 2.4; `re_disasm.txt` L675-737; `orlib_param_orders.md` §4. **Confirmed:** `FindExistPos(name, whType)` returns `ProgId` (>0) if found, `0` if not found. Confirmed for strategies (`whType=0`). ID = record[+0x18]. |
| Subtype lookup | `int __stdcall _FindExistPosSubType@12(const char* name, int WHType, int* outSubType) -> int` | MED | `evidence_C2a_orlib20_lifecycle.md` §2.5; re_report 2.5; `re_disasm2.txt` L342-384. |
| Get master record | `BOOL __stdcall _GetMasterRecByName@12(const char* name, int WHType, void* outRecord)` | HIGH | `evidence_C2a_orlib20_lifecycle.md` §2.3; re_report 2.3; `re_disasm.txt` L451-484. Copies 268-byte (0x42 dwords + 1 word) master record. |
| Create entry | `int __stdcall _CreateEntryEX@24(int whType, int secondary, const char* name, int param4, char param5, int* outErrCode) -> int` | CONFIRMED | `evidence_C2b_orlib20_build.md` §3; `orlib_param_orders.md` §5. **Confirmed order:** `(whType, 0, name, 0, 0, &errCode)`. Returns new `ProgId` on success. `*outErrCode` = 0xBFFE (alloc-fail) / 0xBFFD (create-failed/name-conflict). `secondary=0`, `param4=0`, `param5=0` observed. |
| Set source text (**historical/non-production; unused by current CLI**) | `int __stdcall _SaveTextEX@16(const char* sourceTextAnsi, int whType, int progId, void* passwordMetadataBuffer)` | **CONFIRMED historical ABI** | Tracked correction: `docs/RE/RE_F4_loadtextfileex_findings.md` §5.1–§5.3. p1 = source text (lstrlenA'd and nonzero-checked); p2 = whType; p3 = ProgId; p4 = password/metadata buffer. The current CLI does not call this export; it uses `_SaveModule_New@20` for source text. |
| Start compile | `void StartCompilingProcess(void)` (cdecl, 0 params) | CONFIRMED | `evidence_C2b_orlib20_build.md` §1; `orlib_param_orders.md` §9. Resets error array `[0x1023cea0]` + count `[0x1023cea4]`; record stride 0x129 (297); allocs compiler state `[0x1023ceb0]`. **Required before `VerifyModule_New`.** |
| End compile | `void EndCompilingProcess(int arg)` (__fastcall, 1 arg; pass `0`) | CONFIRMED | `evidence_C2b_orlib20_build.md` §2; `orlib_param_orders.md` §9. Tears down compiler state. **Required after `VerifyModule_New`**; call with `0`. |
| Verify module | `int __stdcall _VerifyModule_New@20(HWND hwnd, int whType, int progId, int flag, void* compilerObj) -> int` | CONFIRMED | `evidence_C2b_orlib20_build.md` §5; `orlib_param_orders.md` §7. **Confirmed:** `(hwnd, whType, progId, 1, compilerObj)`. `compilerObj` is a **`CELCompiler` object (88 bytes), NOT NULL** (see §5.6). **Compiles AND persists the `.ell` file** — this is the function that actually writes the compiled module to disk. Returns `1` on success. **CAVEAT (2026-08-23, updated TVS-L.3):** Under the now-**removed** `FUN_10011230` production patch, the persisted `.ell` output was **runtime-unlinkable** — the patch bypassed `FUN_1008E640` → `SymInfo::AdjustOffsets` → `SymWriter::Save`, so the module was marked VERIFIED despite incomplete link-finalization and failed in TradeStation Desktop with "Unknown link error occurred." The patch has been **completely removed in TVS-L.3**; the real finalization path now runs under the per-thread STA COM prerequisite from TVS-L.2. This row describes the primitive's nominal contract; see §5.6 supersession for the historical broken-patch root cause and the Workstream L remediation. |
| Verify by name (wrapper) | `BOOL VerifyATechnique(const char* name, int WHType) -> BOOL` | HIGH | `evidence_C2b_orlib20_build.md` §5 (`VerifyATechnique` wrapper disasm). Looks up module internally; preferred for CLI. |
| Get errors | `std::string _GetCompilingErrors@4(std::string* hiddenRetPtr)` | CONFIRMED | `evidence_C2b_orlib20_build.md` §7; `orlib_param_orders.md` §10. Builds concatenated "name: msg\n..." across all error records; if array empty, reads `VERIFY.ERR`. An empty buffer = success. **CORRECTED (TVS-A.2/A.4):** The actual ABI is `std::vector<ErrorOutputRecord>*` (3 pointers, 12 bytes), NOT `std::string` return-by-value. The singular `GetCompilingError` (not plural) DOES use std::string hidden return ptr. The production code bypasses GetCompilingErrors entirely and reads the global error array directly. See AGENTS.md DONE entries for TVS-A.2 and TVS-A.4. |
| Get error level | `int _GetErrorLevel@4(int idx) -> int` | HIGH | `evidence_C2b_orlib20_build.md` §8 (`_GetErrorLevel@4` disasm). 1 if record[0]==0x7775 sentinel (error), 0 = warning/none. Error count = global `[0x1023cea4]`. No single "overall level"; caller checks count>0 + per-record levels. |
| Persist/save (source text) | `int __stdcall _SaveModule_New@20(int whType, int progId, void* arg3, HWND editorHwnd, void* arg5) -> int` | CONFIRMED | `evidence_C2b_orlib20_build.md` §6; `orlib_param_orders.md` §6. The current production call is `(whType, progId, &saveFlags, hwnd, savePasswordBuf)`: arg3 and arg5 are both non-NULL; `savePasswordBuf` is zero-initialized first and then populated with the internal default `"!p$Kn@aSus"`, leaving only its unused tail zero. Reads source via `WM_GETTEXT` from an `IsWindow`-checked `EDIT` control, converts wide→UTF-8, sets warehouse source text. **Does NOT persist `.ell`/`.xml` files** — persistence is performed by `VerifyModule_New` (see above). Returns `1` on success. The earlier `(whType, progId, NULL, hwnd, NULL)` call was a superseded pre-M.2 implementation state. |
| Mark deleted (flag only) | `void __stdcall _SetSigDeleted@4(void* masterRecord)` | HIGH | `evidence_C2a_orlib20_lifecycle.md` §2.6; `evidence_C2b_orlib20_build.md` §10; re_report 2.6; `re_disasm.txt` L1127-1141. Sets byte@0x79 bit0, clears word@0xD8 bit1. NO file I/O. |
| Is deleted | `BOOL __stdcall _IsSigDeleted@4(const void* rec)` | HIGH | `evidence_C2a_orlib20_lifecycle.md` §2.7; `evidence_C2b_orlib20_build.md` §10; re_report 2.7; `re_disasm.txt` L1347-1361. |
| File removal | `void FILEDELETE(void* moduleInfo, ?, LPCWSTR path)` (export) | MED | `evidence_C2b_orlib20_build.md` §11 (`FILEDELETE` disasm) + DeleteFile xref map. DeleteFileW + dependent files. |
| Path builder | `void SigNumToFileName(char* outBuf, int WHType, int progId)` | HIGH | `evidence_C2b_orlib20_build.md` (`SigNumToFileName` disasm); `evidence_C2a_orlib20_lifecycle.md` §4.4; re_report 4.4; `re_disasm3.txt`. Result: `<basepath[WHType]><letter><NNNN>` where letter = S/F/I/B/M for WHType 0/1/2/3/4, NNNN = progId zero-padded-4. Base-path table at `0x10243000`. |
| Remove metadata (chained) | `void RemoveMetaDataModules(const char* name, int sigNum)` | MED | `evidence_C2a_orlib20_lifecycle.md` §3.2; re_report 3.2; `re_disasm.txt` L1795-1829. Calls `MDVault::RemoveChainedModules`. Does NOT directly delete files. |
| Remove metadata (single) | `void RemoveMetaDataModule(const char* name)` (unexported) | MED | `evidence_C2a_orlib20_lifecycle.md` §3.3; re_report 3.3; `re_disasm4.txt` L207-296. Calls `MDVault::RemoveModule`. |
| Clean masters | `BOOL __stdcall _CleanMastersEX@4(int WHType)` | MED | `evidence_C2a_orlib20_lifecycle.md` §3.6; re_report 3.6; `re_disasm.txt` L2691-2796. Iterates records, clears changed/linked flag @0xe4. |
| Warehouse singleton | `void* GetWarehouse(const char* nameOrZero)` (Ord 10 @ 0x101A9650) | HIGH | `evidence_C2b_orlib20_build.md` (`GetWarehouse singleton` disasm); `evidence_C2a_orlib20_lifecycle.md` §2.3. Thread-safe; called by nearly every function. |
| Warehouse flush/close | `void __stdcall _CloseWarehouseForCurrentThreadAndPath@4(int arg)` | CONFIRMED | `orlib_param_orders.md` §11. **Confirmed:** call with `0` after all operations to flush/close the warehouse. Required at end of INSERT/MODIFY flow before `OrlibExit`. |
| Direct save (alt) | `MDVault::SaveModule(ModuleInfo*, std::wstring&...)` (ELMDCore.dll, mangled) | LOW (unconfirmed) | `evidence_C2b_orlib20_build.md` §6 (mangled import symbol referenced in `_SaveModule_New` semantics). Bypasses the GUI-bound `_SaveModule_New`. |
| Get source text | `HGLOBAL __stdcall _LoadTextFileEX@16(void* unused, int whType, int progId, void* passwordBuffer)` — returns a global memory handle containing the source text (caller must `GlobalLock`/`GlobalFree`); returns NULL on failure. | CONFIRMED (TVS-F.4) | Named export confirmed via Ghidra (addr 0x101081A0, xref EXTERNAL). Parameter order confirmed via Ghidra static analysis in TVS-F.4 (thunk + authoritative mirror caller FUN_10108630; Frida NOT needed). Implemented in the `get-source` subcommand. Counterpart to `_SaveTextEX@16` (which sets source text). See note below. |

> **Corrected note (`_LoadTextFileEX@16`, 2026-08-23):** Source text is stored in the warehouse (`MWRKArea.wrk`), not in `.ell` files (compiled RNAM modules). Inner logic (`FUN_101a0f90`) performs warehouse lookup, command `0x11`, allocation, and optional decryption; the command dispatcher requires the validated thread-check patch. The earlier "standard/password-protected" interpretation was wrong: library items do not expose user password protection. For internally encrypted storage (master-record flag `0x400`), the get-side path passes a separate 64-byte buffer initialized to zero before the call; the loader then fills it from the embedded password and returns decrypted plaintext. Production `getSource()` now matches that behavior. See `docs/RE/RE_M2_SAVEMODULE_NEW.md` §6 and the dated correction in
> `docs/RE/RE_F4_loadtextfileex_findings.md`.

### 5.2 WHType enum

> **Evidence:** `orlib_param_orders.md` §1. The authoritative type→whType
> mapping (5 confirmed: Strategy, Indicator, Function, TradingApp, ShowMe;
> 3 unconfirmed: ActivityBar, PaintBar, ProbabilityMap), the expanded-whType
> encoding formula, the per-type API category/sub-type decomposition, and the
> `.ell` filename conventions are documented in the SSOT
> `docs/TS_COMPONENT_TYPES.md` (code SSOT: `src/tsdev_lib.h` enum). The
> historical static-survey entries for WHType 1/3, the Indicator subtypes
> (PaintBar/ProbabilityMap etc.), and the WHType 0x32 (50) special case that
> appears in verify/save are **UNCONFIRMED** and are also tracked in
> `docs/TS_COMPONENT_TYPES.md` rather than restated here.

TypeConvert (`0x101d43a0`) expands a WHType into a composite-ID list (subtype<<8 | WHType) terminated by -1, used by warehouse lookups to search all subtypes.

### 5.3 Proposed call sequences (pseudocode)

> **Historical evidence:** The original `orlib_param_orders.md` §15 complete-flow
> record is a gitignored transient artifact and unavailable in the tracked
> checkout. Its INSERT persistence observation was **historical/pre-L.3** and
> patch-based, not current runtime-acceptance evidence. The current production
> flow is represented by the tracked `TsDevLib::saveSource()`,
> `TsDevLib::verifyModule()`, `TsDevLib::deleteEntry()` implementation and the
> current catalog in `docs/SIGNATURE_CATALOG.md`. The dated MODIFY and DELETE
> run claims below are retained as historical/transient records, not current
> acceptance results.

**Historical/pre-L.3 common preamble (patch-based; superseded):**
```cpp
// 1. Set DLL search directory to the TS Program folder
SetDllDirectoryW(L"C:\\Program Files (x86)\\TradeStation 9.5\\Program");

// 2. Load the library engine
HMODULE orlib = LoadLibraryW(L"orlib20.dll");

// 3. Pre-load support DLLs required for compilation (before OrlibInit)
LoadLibraryW(L"ECL.dll");
LoadLibraryW(L"ORELUI20.dll");
LoadLibraryW(L"orUI20.dll");
LoadLibraryW(L"elobjmod.dll");
LoadLibraryW(L"elwinforms.dll");
LoadLibraryW(L"tschartxelobjmdl.dll");
LoadLibraryW(L"ELDebugger.dll");

// 4. Patch FUN_10011230 at orlib20_base+0x11230 (SymWriter bypass — see §5.6)
//    8 bytes: B8 01 00 00 00 C2 0C 00   (MOV EAX,1; RET 0x0C)
//
//    SUPERSEDED 2026-08-23 (Workstream L) / REMOVED in TVS-L.3: This patch
//    step is the bypass that produces runtime-unlinkable output. It has been
//    COMPLETELY REMOVED from production code in TVS-L.3 — the constructor no
//    longer calls applyPersistencePatch(), and the patch byte constants /
//    validation helpers / cached RVA / restoration branch are gone. The real
//    finalization path (FUN_10011230 → FUN_1008E640 → SymInfo::AdjustOffsets
//    → SymWriter::Save) now runs under the per-thread STA COM prerequisite
//    established in TVS-L.2. The `RET 0x0C` is also ABI-invalid: the original
//    callee ends in plain `RET` and the caller performs `ADD ESP,0x0C`
//    (caller cleanup). See §5.6 and docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md.
//    The pseudocode line below is retained as a historical artifact only and
//    is NOT executed by the current production CLI.
patch_memory((BYTE*)orlib + 0x11230, "\xB8\x01\x00\x00\x00\xC2\x0C\x00", 8);

// 5. Init
BOOL ok = OrlibInit(NULL, 1);   // workDirPath = NULL sentinel, contextId = 1
if (!ok) { /* fail loudly */ }
```

**Historical/pre-L.3 INSERT pseudocode (patch-based; superseded):**
```cpp
int whType = 0;  // Strategy (0=Strategy, 2=Indicator, 4=Function — CONFIRMED)
int errCode = 0;

// 6. Check if the component already exists
int progId = FindExistPos(name, whType);   // returns ProgId >0 if found, 0 if not

// 7. If not found, create the entry
if (progId == 0) {
    progId = CreateEntryEX(whType, 0, name, 0, 0, &errCode);
    if (progId <= 0) {
        // errCode 0xBFFE = alloc fail; 0xBFFD = create fail / name conflict
        fail_loudly("insert failed: 0x%X", errCode);
    }
}

// 8. Create a hidden EDIT control and load the source text into it
HWND hEdit = CreateWindowW(L"EDIT", L"", WS_POPUP, 0,0,0,0, NULL, NULL, hInst, NULL);
SetWindowTextW(hEdit, widen(sourceText));

// 9. SaveModule_New — current production passes non-NULL flags and password
//    pointers. The 64-byte password buffer is zeroed first, then its leading
//    10 bytes are populated with TSDev's internal default; only the unused
//    tail remains zero. SaveModule_New saves source text into the warehouse
//    entry (does NOT persist files).
unsigned int saveFlags = (static_cast<unsigned int>(whType) & 0x7u) | 0x60u;
char savePasswordBuf[64] = {};
memcpy(savePasswordBuf, "!p$Kn@aSus", 10);
SaveModule_New(whType, progId, &saveFlags, hEdit, savePasswordBuf);

// 10. Allocate 88-byte CELCompiler and call its constructor (FUN_10165a00, __fastcall, ECX=this)
void* compilerObj = malloc(88);
((void(__fastcall*)(void*))((BYTE*)orlib + 0x165a00))(compilerObj);

// 11. Set compiler fields: +0x10=0xa02, +0x18=hwnd, +0x28=6
*(int*)((BYTE*)compilerObj + 0x10) = 0xa02;
*(HWND*)((BYTE*)compilerObj + 0x18) = hEdit;
*(int*)((BYTE*)compilerObj + 0x28) = 6;

// 12. StartCompilingProcess() — required before VerifyModule_New
StartCompilingProcess();

// 13. VerifyModule_New — compiles AND persists the .ell file
int vok = VerifyModule_New(hEdit, whType, progId, 1, compilerObj);

// 14. GetCompilingErrors — fail loudly if non-empty
std::string errs = GetCompilingErrors(&retPtr);
if (!errs.empty()) { fail_loudly("compile failed: %s", errs.c_str()); }
// NOTE: The `GetCompilingErrors` call in this pseudocode was superseded by direct
// error array access (TVS-A.4). See `src/tsdev_lib.cpp` for the current implementation.

// 15. EndCompilingProcess(0) — required after VerifyModule_New
EndCompilingProcess(0);

// 16. CloseWarehouseForCurrentThreadAndPath(0) — flush/close warehouse
CloseWarehouseForCurrentThreadAndPath(0);

// 17. CELCompiler destructor (FUN_10165ac0, __fastcall, ECX=this)
((void(__fastcall*)(void*))((BYTE*)orlib + 0x165ac0))(compilerObj);
free(compilerObj);

// 18. OrlibExit()
DestroyWindow(hEdit);
OrlibExit();

// 19. Verify elmodule_<whType>_<progId>.ell exists on disk
//     (e.g. elmodule_0_2572.ell, magic RNAM)
```

**Historical MODIFY implementation record — IMPLEMENTED & APPROVED (composes existing INSERT primitives; see §5.8):**
```cpp
int whType = 0;
// 6. Look up the existing component
int progId = FindExistPos(name, whType);
if (progId == 0) { notify_not_found(name); return; }
// 8-17. Same as INSERT from step 8 onward (hidden EDIT control, current
//       non-NULL SaveModule_New flags/password arguments, CELCompiler setup,
//       StartCompilingProcess, VerifyModule_New, error check,
//       EndCompilingProcess, CloseWarehouseForCurrentThreadAndPath, dtor, OrlibExit)
// 19. Verify elmodule_<whType>_<progId>.ell exists on disk
```

> **MODIFY test evidence (2 independent runs, exit 0, all 8 phases pass):**
> - ProgId preserved across modify (2609→2609, 2610→2610).
> - `.ell` timestamp changed on modify (re-compiled + re-persisted).
> - Negative test (nonexistent name) returns `success=false`.
> - Compile-error test returns `success=false` with a non-empty message.
> - Cleanup (`deleteEntry`) passes; no crashes/WerFault.
>
> **Known follow-up (pre-existing, not a MODIFY bug):** `GetCompilingErrors` returns an empty buffer in headless mode; `verifyModule` has a fallback that synthesizes a message when `VerifyModule_New != 1`. This affects error-message fidelity, not correctness of the modify/verify outcome.

**Historical DELETE implementation record — IMPLEMENTED & APPROVED (orchestrator-based; see §5.7):**
```cpp
int whType = expectedType;

// 6. Look up the existing component
int progId = FindExistPos(name, whType);
if (progId <= 0) { notify_not_found(name); return; }

// 7. Call the internal delete orchestrator (FUN_101a8360, RVA 0x1A8360)
//    __fastcall: ECX = whType, EDX = progId
//    SEH-protected. Internally performs DeleteFromTree → closeWarehouse →
//    commit storages, which persists the deletion to MWRKArea.wrk.
//    Requires 3 thread-check INT3 patches (NOP'd in TsDevLib constructor):
//      RVA 0x1D0993, 0x1CF5D4, 0x1D1FC8  (see §5.7)
typedef void (__fastcall *DeleteOrchestratorFn)(int whType, int progId);
DeleteOrchestratorFn deleteOrchestrator =
    (DeleteOrchestratorFn)((BYTE*)orlib + 0x1A8360);
__try {
    deleteOrchestrator(whType, progId);
} __except (EXCEPTION_EXECUTE_HANDLER) {
    fail_loudly("delete orchestrator raised an exception");
}

// 8. Delete .ell / .xml files for the removed entry
char path[MAX_PATH]; SigNumToFileName(path, whType, progId);
FILEDELETE(/*moduleInfo*/, /*p2*/, widen(path));   // deletes .ell + dependents

// 9. OrlibExit()
OrlibExit();
```

> **RETIRED approaches (do NOT use):** The earlier static-analysis DELETE
> pseudocode (`SetSigDeleted` + `RemoveMetaDataModules` + `CleanMastersEX`)
> and all prior persistence attempts (command `0x45`, dirty-flag manipulation
> at offset `+0xBE` with command `0x27`, killing `WHServer.exe`, manual worker
> command sequencing) have been **rejected** — they either crashed, did not
> persist, or violated the standalone constraint. See §5.7 and the unavailable historical record
> `tmp/re_artifacts/DELETE_WORKING_MEMORY.md` for the full rejection record.

### 5.4 SaveModule_New — source text only (no file persistence)

> **SUPERSESSION NOTE — 2026-08-23 (updated TVS-L.3 — patch removed):** The
> phrase "with the `FUN_10011230` patch applied" below references the bypass
> that produced runtime-unlinkable output. The persistence claim (`.ell` is
> written) is accurate at the warehouse/compile-persistence layer, but the
> resulting `.ell` was **runtime-unlinkable** because the patch skipped the
> original compiler/link-finalization path. The patch has been **completely
> removed in TVS-L.3**; the real finalization path now runs under the
> per-thread STA COM prerequisite from TVS-L.2. See §5.6 supersession banner
> and `docs/workstream_unknown_link_fix_plan.md`.

`_SaveModule_New@20` is GUI-bound in its source-retrieval path: it reads source via `WM_GETTEXT` from an `IsWindow`-checked EDIT control, converts wide→UTF-8, and sets the warehouse source text. **It does NOT persist `.ell`/`.xml` files** — it saves source text into the warehouse entry only (see §5.1 primitive table). Persistence of the compiled `.ell` file is performed by `VerifyModule_New` (historically "with the `FUN_10011230` patch applied — see §5.6"; that patch has been **completely removed in TVS-L.3**, and `VerifyModule_New` now drives the real finalization path `FUN_1008E640` → `SymInfo::AdjustOffsets` → `SymWriter::Save` → `ModuleInfo::SetGuid` / `MDVault::SaveModule` under the per-thread STA COM prerequisite from TVS-L.2), which compiles the source and calls `ELMeta::MDVault::SaveModule` to write `elmodule_<X>_<N>.ell` to disk.

**Hidden-EDIT workaround (current implementation; historical demonstration):** Create a hidden `EDIT` control (`WS_POPUP`, zero-size), `SetWindowTextW` it with the source, and pass its HWND as `editorHwnd`. This satisfies the `IsWindow` + `WM_GETTEXT` path without a visible UI. The retained standalone INSERT demonstration is historical/transient (see the historical §5.3 and §5.6 records).

> **NOTE:** An earlier version of this section stated that `_SaveModule_New` "calls `MDVault::SaveModule` (which writes `elmodule_<X>_<N>.ell/.xml`)." That was incorrect — `_SaveModule_New` sets warehouse source text only. File persistence is via `VerifyModule_New` → `CompileText` → `MDVault::SaveModule`; the `patched CompileText` wording describes only the historical pre-L.3 bypass and is superseded (see historical §5.6).

### 5.5 Remaining unknowns / blockers

> **SUPERSESSION NOTE — 2026-08-23 (updated TVS-L.3 — patch removed):** The
> "Resolved items" sentence below lists "the GUI-save persistence path
> (resolved via the `FUN_10011230` patch — see §5.6)" as a resolved item. That
> resolution was **superseded** for runtime-link acceptance: the patch produced
> runtime-unlinkable output, and has now been **completely removed in TVS-L.3**.
> See §5.6 supersession banner and `docs/workstream_unknown_link_fix_plan.md`.

> **Historical resolved-items record:** The following were listed as unknowns and
were recorded as **CONFIRMED** at the time — see §5.1 and the historical §5.6:
`_CreateEntryEX` params 2-5, `_VerifyModule_New` p3/p4/p5, `_SaveModule_New`
GUI dependency (hidden-EDIT workaround), `_SaveTextEX` persistence semantics,
`_GetCompilingErrors` formatting, `contextId=1`, and the GUI-save persistence
path as exercised with the `FUN_10011230` patch. The patch-based persistence
record is historical/pre-L.3 and superseded; current source/catalog evidence is
used for production status.

**Genuinely unresolved:**

1. **Warehouse type mappings for ActivityBar / PaintBar / ProbabilityMap** — not confirmed; do NOT treat as known (see §5.2). Any code today must restrict itself to the five confirmed types (authoritative type→whType mapping in `docs/TS_COMPONENT_TYPES.md`).
2. ~~**DELETE flow not yet tested end-to-end**~~ — **RESOLVED — IMPLEMENTED & APPROVED.** The orchestrator-based approach (`FUN_101a8360`) persists deletions to `MWRKArea.wrk` standalone, verified by two independent test runs (see §5.7). Prior approaches (`SetSigDeleted` + `RemoveMetaDataModules` + `CleanMastersEX`, command `0x45`, dirty-flag manipulation, `WHServer.exe` termination) are RETIRED/rejected.
3. **Historical/non-production `_SaveTextEX@16` ABI:** the tracked correction confirms p1 = source text, p2 = whType, p3 = ProgId, and p4 = password/metadata buffer (`docs/RE/RE_F4_loadtextfileex_findings.md` §5.1–§5.3). `_SaveTextEX@16` is unused by the current CLI; the current source-text path uses `_SaveModule_New@20` instead.
4. **Error record field layout** (stride 297) beyond the 0x7775 sentinel — unmapped.
5. **WHType 0x32 special case** — meaning unknown; may affect verify/save.

### 5.6 Historical pre-L.3 persistence observation — SymWriter bypass (superseded; see banner)

> **SUPERSESSION BANNER — 2026-08-23 (Workstream L root-cause confirmation).**
> The narrative in this section is **superseded** in three places (corrections
> applied in-line below; historical text preserved with correction rather than
> destructive rewriting):
>
> 1. **Root cause ("partially initialized CLR") — SUPERSEDED / WRONG.** The
>    real prerequisite is **per-thread STA COM** for the MSXML COM wrapper used
>    by `SymWriter::Save`. TSDev's UI/main initialization thread statically
>    calls `CoInitialize(NULL)` (STA) before `_OrlibInit(NULL,1)` on the same
>    synchronous path; the CLI's 64 MiB worker threads never initialize COM, so
>    the unpatched standalone path raised native C++ exception `0xE06D7363`.
>    The "partially initialized CLR" explanation was unproven and is wrong.
> 2. **Fix ("patch-as-fix") — SUPERSEDED.** The patch is not a fix; it is the
>    **bypass that causes the runtime-unlinkable output.** Durable Ghidra
>    evidence (`docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`) proves the original
>    function's ordered work includes a call to `FUN_1008E640`
>    (compiler/link-finalization), `SymInfo::AdjustOffsets`, and
>    `SymWriter::Save`. The patch skips all of that and forces `EAX=1`, so
>    `CompileText` proceeds to `ModuleInfo::SetGuid` / `MDVault::SaveModule`
>    and marks the item verified despite an incomplete finalization. A
>    controlled dynamic A/B (`tmp/com_unpatched_probe/`,
>    `tmp/com_nopatch_6689.ell`, `tmp/com_nopatch_6689.xml`) proved the
>    combined fix: STA COM on the worker + restored original `FUN_10011230`
>    bytes produced a paired ELL/XML with matching embedded version GUID, and
>    the user confirmed the strategy ran in Desktop without opening/re-verifying
>    in TSDev.
> 3. **ABI claim ("`__fastcall` with 5 parameters ... hence `RET 0x0C`") —
>    SUPERSEDED.** Durable Ghidra evidence shows the original callee ends in
>    **plain `RET`** at `0x10011454` and the caller executes **`ADD ESP,0x0C`**
>    at `0x1001E0C1`. The original ABI is fastcall-like register passing with
>    **caller cleanup** for the 12 stack bytes. The patch's `RET 0x0C` changes
>    that cleanup contract to callee cleanup — a static ABI hazard, not a
>    correct ABI match. No claim is made that an observed historical crash was
>    caused by it.
>
> **HISTORICAL TVS-L.3 UPDATE — 2026-08-23 (persistence bypass removed):** The
> `FUN_10011230` persistence patch has been **completely removed** from
> production code in TVS-L.3. The real finalization path
> (`FUN_10011230` → `FUN_1008E640` → `SymInfo::AdjustOffsets` →
> `SymWriter::Save`, then `ModuleInfo::SetGuid` / `MDVault::SaveModule`) now
> runs end-to-end under the per-thread STA COM prerequisite established in
> TVS-L.2. The historical "still present in production code and is slated for
> complete removal in TVS-L.3 ... No implementation has been performed"
> statement immediately below is **superseded** by this completion note
> (historical text preserved rather than destructively rewritten). The former
> `FUN_10011230_persistence_patch` signature entry and its patch site are gone;
> the post-L.3/pre-M.1 catalog phase and current tracked totals (signature
> definitions, tracked exports, data globals, and patch sites) are documented
> in the SSOT `docs/SIGNATURE_CATALOG.md` (current and historical
> resolver/catalog counts).
>
> The historical text immediately below (preserved) read: "The patch is still
> present in production code and is slated for **complete removal** in TVS-L.3
> (`docs/workstream_unknown_link_fix_plan.md`), after TVS-L.2 establishes the
> STA COM prerequisite that makes the unpatched path safe to run. No
> implementation has been performed."

> **Historical evidence (transient/unavailable in the tracked checkout):** `orlib_param_orders.md` §12. This pre-L.3 patch observation showed standalone disk persistence of `.ell` files, but it was not runtime-link acceptance. The current production source leaves the target unpatched; see `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md` §C and `docs/SIGNATURE_CATALOG.md` current totals.
>
> **SUPERSEDED 2026-08-23 (Workstream L, updated TVS-L.3 — patch removed):**
> The "critical fix" referenced here is **only historical persistence
> evidence** — it demonstrated that `.ell` files could be written standalone —
> and is **not the production fix.** As detailed in the section banner above,
> the patch was the bypass that produced runtime-unlinkable output (Desktop
> "Unknown link error occurred"); the real production remediation is
> Workstream L (`docs/workstream_unknown_link_fix_plan.md`). TVS-L.1 and
> TVS-L.2 are COMPLETE, and TVS-L.3 has **completely removed** the patch from
> production code so the real finalization path now runs under the per-thread
> STA COM prerequisite. Treat this Evidence preamble as a historical artifact,
> not a runtime-acceptance statement.

**Root cause:** `SymWriter::Save` in **`ELDebugger.dll`** (at RVA `0x120a0`) calls `XmlDocument.Save` — a .NET managed method — via a virtual call at vtable offset `0xd8`. In a standalone native process, the CLR is only **partially** initialized: the mixed-mode C++/CLI support DLLs load, but they do not fully boot the runtime, so the JIT-compiled target address is invalid. The virtual call therefore causes an access violation.

> **SUPERSEDED 2026-08-23:** The "partially initialized CLR" explanation above
> is **wrong/unproven**. The real prerequisite is **per-thread STA COM** for
> the MSXML COM wrapper used by `SymWriter::Save`. See the section banner and
> `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`.

That exception is caught by the SEH handler of `FUN_10011230` in **orlib20.dll**. When the handler runs, `CompileText` skips `ELMeta::MDVault::SaveModule` — the function that actually writes the `.ell` file. As a result, the source compiles but **nothing is persisted to disk**.

**Fix:** Patch `FUN_10011230` in orlib20.dll (at RVA `0x11230`) to **return 1 immediately**. This bypasses the `SymWriter` crash entirely. `CompileText` then proceeds normally to call `ELMeta::MDVault::SaveModule`, which writes the `.ell` file (compiled module, magic `RNAM`).

> **SUPERSEDED 2026-08-23 (updated TVS-L.3 — patch removed):** The "Fix" above
> is not a fix; it is the **bypass that causes the runtime-unlinkable output.**
> The patch skips `FUN_1008E640` (compiler/link-finalization),
> `SymInfo::AdjustOffsets`, and `SymWriter::Save`, then forces `EAX=1` so
> `CompileText` proceeds to `ModuleInfo::SetGuid` / `MDVault::SaveModule` and
> marks the item verified despite an incomplete finalization. The real fix is
> per-thread STA COM (TVS-L.2, COMPLETE) + complete patch removal (TVS-L.3,
> **COMPLETE** — the patch, its byte constants, validation helpers, constructor
> call, cached RVA/original-byte members, and destructor restoration branch
> have all been removed from production code). See the section banner and
> `docs/workstream_unknown_link_fix_plan.md`.

**Patch bytes (8 bytes at `orlib20_base + 0x11230`):**
```
B8 01 00 00 00    MOV EAX, 1
C2 0C 00          RET 0x0C
```
`FUN_10011230` is `__fastcall` with 5 parameters (`ECX`, `EDX`, and 3 stack args = 12 bytes), hence `RET 0x0C`. The patch is applied **in-process to the loaded orlib20.dll image in the standalone CLI's own address space** (memory only); the on-disk `orlib20.dll` is never modified (READ-ONLY install preserved).

> **SUPERSEDED 2026-08-23 (ABI claim):** The "hence `RET 0x0C`" justification
> above is **wrong**. Durable Ghidra evidence
> (`docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`) shows the original callee ends in
> **plain `RET`** at `0x10011454` and the caller executes **`ADD ESP,0x0C`** at
> `0x1001E0C1`: the original ABI is fastcall-like register passing with
> **caller cleanup** for the 12 stack bytes. The patch's `RET 0x0C` changes
> that cleanup contract to callee cleanup — a static ABI hazard, not a correct
> ABI match. No claim is made that an observed historical crash was caused by
> it. The in-process-only / READ-ONLY-install statement remains accurate.

**Consequence:** The `.xml` file (debug symbols) is **NOT** written. This is acceptable — 7 existing `.ell` files in `MyWork\Bin` already operate without paired `.xml` files. The `.ell` file is the compiled module that TradeStation actually uses.

> **SUPERSEDED 2026-08-23:** The "XML not written is acceptable" claim above
> is **superseded**. XML itself is debugger/source-map data and is **not
> consumed by the runtime linker**, so XML absence is not *directly* the link
> failure. However, XML absence is a **visible marker of the bypass**: the
> causal defect is the bypassed original compiler/link-finalization path
> (`FUN_1008E640` → `SymInfo::AdjustOffsets` → `SymWriter::Save`) producing
> runtime-unlinkable output. The "7 existing unpaired `.ell` files" observation
> was existence-only and did not prove executable strategies work without XML.
> See the section banner and `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`.

**Evidence:** Standalone probe `elevated_test20.exe` with the in-process patch produces `elmodule_0_2572.ell` (5091 bytes, magic `RNAM`) with **no Frida attached**. Compilation error buffer is empty. `VerifyModule_New = 1`.

> **SUPERSEDED 2026-08-23:** The probe evidence above demonstrates
> *warehouse/compile-persistence only*; it does **not** demonstrate
> runtime-link acceptance. Confirmed Unknown-link findings show CLI-produced
> strategies fail in TradeStation Desktop with "Unknown link error occurred"
> despite this probe succeeding. The dynamic A/B in
> `tmp/com_unpatched_probe/` is the durable proof of the corrected
> root cause and fix direction.

### 5.7 Historical DELETE implementation record — orchestrator approach

> **Historical evidence (transient/unavailable in the tracked checkout):**
> `tmp/re_artifacts/DELETE_WORKING_MEMORY.md` (the original production section).
> The retained adversarial review was recorded as APPROVED; current delete
> behavior is represented by `TsDevLib::deleteEntry()` and the tracked source.

**Approach:** DELETE calls the internal orlib20 **delete orchestrator** `FUN_101a8360` (RVA `0x1A8360`) — the same high-level routine TSDev.exe's own delete path drives. It encapsulates the full worker-command sequence (open warehouse → delete + commit storages → update BST → close) so the CLI does not replicate individual worker commands.

**Delete flow (verified):**
1. `FindExistPos(name, whType)` → `progId` (exported, safe; notify if not found).
2. `FUN_101a8360(whType, progId)` — `__fastcall` (`ECX=whType`, `EDX=progId`), SEH-protected. Internally performs `DeleteFromTree` → `closeWarehouse` → commit storages, which persists the deletion to `MWRKArea.wrk`.
3. Delete `.ell`/`.xml` files for the removed entry (`SigNumToFileName` + `FILEDELETE`).
4. `OrlibExit()`.

**Thread-check INT3 patches:** Three `swi(3)` (INT3) thread-check instructions in the orchestrator path would crash a standalone CLI (calling thread ≠ orlib worker thread). They are NOP'd out in-process (loaded image only; on-disk DLL never modified), applied once in the `TsDevLib` constructor:

| RVA | Patch |
|-----|-------|
| `0x1D0993` | `INT3` → `NOP` |
| `0x1CF5D4` | `INT3` → `NOP` |
| `0x1D1FC8` | `INT3` → `NOP` |

**Test evidence (2 independent runs):** Exit code 0 (both runs); no WerFault/crash; `MWRKArea.wrk` shrank 4096 bytes with changed timestamp; fresh `FindExistPos` in a new process returned −1; `.ell` removed.

**Standalone constraint maintained:** No TSDev.exe required, no `WHServer.exe` killing. The READ-ONLY TradeStation install is never modified (patches are in-process to the loaded DLL image only).

**Retired/rejected approaches (do NOT use):** Direct lookup path (`FUN_101d05e0` — crashed); `CloseWarehouseForCurrentThreadAndPath` alone (did not persist); dirty-flag manipulation at offset `+0xBE` with command `0x27` (command `0x27` is not a save — it is a thin COM wrapper with no file I/O); command `0x45` / commit-all-storages (accepted but did not persist standalone); killing `WHServer.exe` (did not persist, caused crash-report popups); manual worker command sequencing `0x0F`→`0x32`→`0x17` (superseded by the orchestrator). See the unavailable historical record `tmp/re_artifacts/DELETE_WORKING_MEMORY.md` for the full historical record.

### 5.8 Historical MODIFY implementation record — composition of INSERT primitives

> **Historical/transient evidence:** Two independent test runs were recorded as
> exit 0 with all 8 phases passing; the retained adversarial review was APPROVED.
> These results are not current acceptance evidence.

**Approach:** MODIFY introduces **no new orlib20 calls and no new RVAs**. It composes only existing, already-confirmed INSERT primitives in sequence:

1. `findExistPos(name, whType)` → `progId` (notify if not found; `success=false`).
2. `saveSource` — hidden EDIT control + `SaveModule_New` (sets warehouse source text for the existing entry).
3. `verifyModule` — `CELCompiler` setup + `StartCompilingProcess` + `VerifyModule_New` (historically "with the `FUN_10011230` patch applied — see §5.6"; that patch has been **completely removed in TVS-L.3**, and `VerifyModule_New` now drives the real finalization path under the per-thread STA COM prerequisite from TVS-L.2) → compiles AND re-persists the `.ell` file for the existing `progId`.
4. Error check + `EndCompilingProcess` + `CloseWarehouseForCurrentThreadAndPath` + `OrlibExit`.

The only difference from INSERT is that `CreateEntryEX` is skipped — the existing `progId` is reused, so the ProgId is preserved across the modify.

**Test evidence (2 independent runs, exit 0, all 8 phases pass):**
- ProgId preserved across modify (2609→2609, 2610→2610).
- `.ell` timestamp changed on modify (re-compiled + re-persisted).
- Negative test (nonexistent name) returns `success=false`.
- Compile-error test returns `success=false` with a non-empty message.
- Cleanup (`deleteEntry`) passes; no crashes/WerFault.

**Known follow-up (pre-existing, not a MODIFY bug):** `GetCompilingErrors` returns an empty buffer in headless mode; `verifyModule` has a fallback that synthesizes a message when `VerifyModule_New != 1`. This affects error-message fidelity, not the correctness of the modify/verify outcome.

---

## 6. Comparison & Recommendation

> **SUPERSESSION NOTE — 2026-08-23 (updated TVS-L.3 — patch removed):** The
> Recommendation below characterizes the `FUN_10011230` patch as "persistence
> ... demonstrated" and "LOW risk." That characterization is **superseded**:
> the patch was the bypass that produced runtime-unlinkable output, and
> production runtime acceptance was **BROKEN** while the patch was present.
> The patch has now been **completely removed in TVS-L.3**; the real
> finalization path runs under the per-thread STA COM prerequisite from
> TVS-L.2. The Approach 2 recommendation (orlib20.dll direct) remains the
> correct architectural choice; the patch was the implementation defect
> removed by Workstream L (`docs/workstream_unknown_link_fix_plan.md`). See
> §5.6 supersession banner.

| Criterion | Approach 1 (ELIS.exe COM) | Approach 2 (orlib20.dll direct) |
|-----------|---------------------------|---------------------------------|
| Name -> (WHType, ProgId) resolution | NOT exposed; must scan XML out-of-band | `FindExistPos` / `_GetMasterRecByName` (HIGH conf) |
| Raw-source insert | No — XML file import only | `_CreateEntryEX` + `_SaveModule_New` (CONFIRMED) |
| Compile/verify | `VerifyAll` (whole library, void) | `StartCompilingProcess` + `VerifyModule_New` (CONFIRMED) |
| Error reporting | Void on delete/verify; results BSTR on import only | `_GetCompilingErrors` + `_GetErrorLevel` + count global (CONFIRMED) |
| Delete | `DeleteAnalysisTechnique(WHType,ProgId)` void; needs ProgId | Historical implementation record: orchestrator `FUN_101a8360` (`__fastcall`, SEH-protected) + thread-check INT3 NOP patches + `FILEDELETE` (see historical §5.7) |
| Modify | No primitive (delete+insert) | Historical implementation record: set source + verify + save (same primitives as insert; see historical §5.8) |
| Complexity / risk | Low call complexity, HIGH gap-filling complexity (XML synth, name resolution, error inference) | Historical study characterized this as MED call complexity; parameter orders were recorded CONFIRMED, while patch-based persistence was pre-L.3 and is superseded |
| Headless viability | Uncertain (Desktop coupling suspected; void errors) | Historical smoke-test result was PASS; no current run is implied |

### Recommendation

**Primary: Approach 2 (orlib20.dll direct invocation).** It is the only approach that exposes primitives for all three acceptance criteria, including targeted compile/verify with retrievable errors and name resolution. Parameter orders for the key primitives are **CONFIRMED**. The retained standalone INSERT persistence observation was **historical/pre-L.3 and patch-based** (see historical §5.6), not current runtime acceptance; current code/catalog evidence records the persistence-patch removal. **DELETE and MODIFY implementation records are historical/transient** (see §5.7 and §5.8), not current acceptance claims. Remaining risks are narrow: warehouse-type mappings for three component types (ActivityBar, PaintBar, ProbabilityMap) and minor unmapped fields (see §5.5). It aligns with the project goal of least custom implementation: the CLI becomes a thin orchestrator over the same engine TSDev.exe uses.

**Fallback: Approach 1 (ELIS.exe COM).** Retain for the delete operation only, and only if name resolution is handled externally (e.g., XML scan) and the void return is acceptable. Not viable for insert/modify due to XML-only import and absent error reporting.

Both de-risking gates were recorded as **PASSED in the historical study**: (a)
`orlib20` parameter orders are documented in §5.1, and (b) the headless
`_OrlibInit` smoke test succeeded without TradeStation Desktop running. The
referenced `headless_init_smoke_test.md` is a gitignored transient artifact and
unavailable in the tracked checkout; no current run is implied.

---

## 7. Next Steps (TVS-2 Proposal)

> **SUPERSESSION NOTE — 2026-08-23 (updated TVS-L.3 — patch removed):** The
> status updates below mark the `FUN_10011230` patch path as "COMPLETED" /
> "DONE." That was accurate for warehouse/compile-persistence only and was
> **NOT** runtime-link acceptance. Production runtime acceptance was
> **BROKEN** while the patch was present; the patch was the bypass removed by
> Workstream L (`docs/workstream_unknown_link_fix_plan.md`, TVS-L.1 through
> TVS-L.5). TVS-L.1 (RED harness) and TVS-L.2 (STA COM lifecycle guard) are
> COMPLETE, and TVS-L.3 has **completely removed** the persistence patch from
> production code so the real finalization path now runs. See §5.6 supersession
> banner.

> **Historical status update:** Items (a), (b), and (c) below were recorded as
> **COMPLETED** at the time of this feasibility report. The parameter-order and
> headless-init results, the patch-based INSERT persistence observation, and the
> DELETE/MODIFY implementation records are historical/transient; they are not
> current acceptance evidence. Current production status is maintained by the
> tracked source, catalog, and current workstream documents. Remaining work and
> limitations are in §5.5 and §8.

The highest-ROI next slice de-risks the recommendation before any broad implementation:

**a) Resolve orlib20 unknown parameter orders (RE) — COMPLETED.**
**Historical/superseded MCP procedure:** The original primary approach used **live tracing via a Python frida MCP server** (Frida 17.17.0): it spawned or attached to TSDev.exe via `create_interactive_session`, hooked the orlib20 functions (`_CreateEntryEX@24`, `_SaveTextEX@16`, `_SaveModule_New@20`, `_VerifyModule_New@20`) with `Interceptor.attach` to capture `args[0..N]` in `onEnter`, then triggered insert/delete/modify operations in the TSDev UI to observe the actual argument values passed by the authoritative caller. This gives the historical dynamic observations retained by the study; the MCP procedure is not current and must not be used.

Secondary/confirmatory approach — **RE the caller sites in TSDev.exe** (static, deterministic): disassemble the call sites of the same functions within TSDev.exe to recover the exact argument order and types the GUI host uses. Use this to corroborate the live-trace findings.

**Historical/superseded MCP procedure:** As a quick preliminary to the headless smoke test (step b), the study hooked `_OrlibInit@8` in ELIS.exe at startup via the frida MCP to capture its actual arguments (`workDirPath`, `contextId`) — this resolved open question #2 (`contextId` valid values) from the authoritative caller.

**Current direct-CLI guidance:** Follow-up runtime instrumentation uses Frida directly from the CLI — `frida-trace`, `frida -l script.js`, or `frida -p PID` — and never Frida MCP, as required by `AGENTS.md`. The static caller-site RE above remains the confirmatory method for argument order and types.

**b) Headless init smoke test (critical gate) — COMPLETED (PASSED).**
The historical smoke procedure built a minimal 32-bit C++ test that loaded
orlib20.dll, set the DLL search directory to the TS Program folder, and called
`_OrlibInit(workDirPath, contextId)` with candidate `contextId` values. It
asserted a nonzero return and `ELRte::CreateRte` success via a subsequent
`FindExistPos` call. That historical run used no TradeStation Desktop; this is
not a general current Desktop-state prerequisite. The result was treated as
the critical headless-viability gate for the feasibility study.

**c) Historical CLI TDD record (red-green) — COMPLETED at study time.**
1. ~~**DELETE**~~ — **DONE & APPROVED.** Orchestrator-based approach (`FUN_101a8360`) with thread-check INT3 NOP patches; persistence verified by two independent test runs (see §5.7).
2. ~~**INSERT**~~ — **Historical/pre-L.3 DONE record.** Standalone persistence was demonstrated via `elevated_test20.exe` + `FUN_10011230` patch (see historical §5.6); the current production path is represented by the unpatched source/catalog.
3. ~~**MODIFY**~~ — **DONE & APPROVED.** Composes only existing INSERT primitives (`findExistPos` → `saveSource` → `verifyModule`) with no new orlib20 calls and no new RVAs; verified by two independent test runs (exit 0, ProgId preserved, `.ell` updated, compile errors surfaced, negative + compile-error tests return `success=false`; see §5.8).

**Historical planning language (superseded):** Each function was planned to get a red-green TDD cycle against a disposable test library copy (never the real install).

**Current L7 behavior (repository evidence):** The opt-in `-IncludeRedFaultInjection` path targets the configured TradeStation installation/MyWork path (by default the real installation) and mutates only its exact L7 artifacts to exercise Fault A/B; it is not part of normal regression. The current harness derives `<TsPath>\MyWork\Bin` when `-TsPath` is supplied and uses `C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin` when it is omitted; see `AGENTS.md` and `tests/test_unknown_link_regression.ps1`.

---

## 8. Open Questions / Risks

> **SUPERSESSION NOTE — 2026-08-23 (updated TVS-L.3 — patch removed):**
> Resolved items #5, #6, and #11 below mark the `FUN_10011230` patch path as
> "RESOLVED." That resolution was **superseded** for runtime-link acceptance:
> the patch produced runtime-unlinkable output. The real prerequisite is
> per-thread STA COM (not "partially initialized CLR"), and the patch has been
> **completely removed in TVS-L.3** (no longer "slated for" removal — the
> removal is done). See §5.6 supersession banner and
> `docs/workstream_unknown_link_fix_plan.md`.

> **Resolved since initial study:** Items #1 (headless `CreateRte` — smoke test PASSED), #2 (`contextId` — CONFIRMED as 1), #3 (`_CreateEntryEX` params — CONFIRMED, see §5.1), #5 (`_SaveModule_New` GUI binding — hidden-EDIT workaround CONFIRMED, historical/pre-L.3 patch-based persistence via `VerifyModule_New` + §5.6 (superseded)), #6 (`MDVault::SaveModule` direct — no longer needed; historical/pre-L.3 patch-based persistence via `VerifyModule_New` → patched `CompileText` → `MDVault::SaveModule` (superseded)), #8 (`_GetCompilingErrors` formatting — CONFIRMED, see §5.1), #11 (`_SaveTextEX` persistence — RESOLVED; `SaveModule_New` saves source only, `VerifyModule_New` persists `.ell`), and #13 (**historical MCP result; procedure superseded:** Frida live RE was enabled via Python frida MCP, Frida 17.17.0).

**Still open:**

1. ~~**Headless `CreateRte`**: Does `ELRte::CreateRte` succeed without TradeStation Desktop?~~ **Historical study result — RESOLVED: YES** (smoke test recorded as PASSED; `headless_init_smoke_test.md` is transient and unavailable in the tracked checkout).
2. ~~**`contextId` valid values`**: What integer(s) are valid for `_OrlibInit`'s second argument?~~ **RESOLVED — `contextId = 1`** (CONFIRMED; see §5.1).
3. ~~**`_CreateEntryEX` params 2-5**: Exact order/meaning of WHType and flags.~~ **RESOLVED — CONFIRMED** `(whType, 0, name, 0, 0, &errCode)` (see §5.1).
4. **Historical/non-production `_SaveTextEX@16` ABI:** the tracked correction confirms p1 = source text, p2 = whType, p3 = ProgId, and p4 = password/metadata buffer (`docs/RE/RE_F4_loadtextfileex_findings.md` §5.1–§5.3). It is unused by the current CLI; `_SaveModule_New@20` is the current source-text path.
5. ~~**`_SaveModule_New` GUI binding`: Does the hidden-EDIT workaround fully suppress the MessageBox?~~ **RESOLVED in the historical study — hidden-EDIT workaround was recorded working** in the standalone INSERT flow (see historical §5.3, §5.6). Current persistence is via `VerifyModule_New`, not `_SaveModule_New`.
6. ~~**`MDVault::SaveModule` direct**: Feasible as a GUI-free alternative?~~ **RESOLVED — not needed.** Historical/pre-L.3 persistence was achieved in the study via `VerifyModule_New` → patched `CompileText` → `MDVault::SaveModule`; this patch-based result is superseded by the current unpatched source/catalog (see §5.6).
7. ~~**Persistence after `_SetSigDeleted`**: Which call persists the master index with the deleted flag before `FILEDELETE` removes files?~~ **RESOLVED — DELETE IMPLEMENTED & APPROVED.** The orchestrator `FUN_101a8360` handles persistence internally (`DeleteFromTree` → `closeWarehouse` → commit storages → `MWRKArea.wrk` written). The `SetSigDeleted`-based approach is RETIRED. See §5.7.
8. ~~**`_GetCompilingErrors` formatting**: Exact string format for error reporting.~~ **RESOLVED — CONFIRMED** (builds concatenated "name: msg\n..." across all error records; empty buffer = success; see §5.1).
9. **Error record layout** (stride 297): Fields beyond the 0x7775 sentinel are unmapped. — **STILL OPEN**.
10. **WHType 0x32**: Meaning unknown; may affect verify/save. — **STILL OPEN**.
11. ~~**`_SaveTextEX` persistence`: Does it persist alone, or is `_SaveModule_New` always mandatory?~~ **RESOLVED — `_SaveModule_New` saves source text only; `VerifyModule_New` persists the `.ell` file** (see §5.1, §5.6).
12. **Registry dependence**: orlib20 uses `orshr20.dll!_TSGetRegistryRootKeyA` and ADVAPI32 registry functions; the TS install's registry keys must be present. — **STILL OPEN**.
13. ~~**Frida live RE**: Live RE was initially blocked by a Frida helper-path bug.~~ **HISTORICAL RESOLUTION (MCP workflow superseded):** The study recorded live RE enabled via a Python frida MCP server running Frida 17.17.0 (which included the fix), providing the full Frida JS API via `execute_in_session` for runtime value confirmation. **Current guidance:** use direct CLI `frida-trace`, `frida -l script.js`, or `frida -p PID`; do not use Frida MCP (see `AGENTS.md`).

---

## 9. Evidence Index

The following names are historical/transient artifacts that were stored under
`tmp\re_artifacts\`; they are gitignored, not checked in, and unavailable as
tracked repository evidence. No replacement files or current results are implied.

| Historical artifact name | Contents |
|------|----------|
| `evidence_A_filesystem_survey.md` | TradeStation install + MyWork\Bin layout, .ell RNAM magic, file counts, DLL inventory. |
| `evidence_A2_binary_static_survey.md` | PE imports/exports, orlib20 export list, ELIS strings/ProgID/CLSID, .ell header bytes, XML schema, WHType names. |
| `evidence_C1_elis_typelib.md` | ELIS COM registration + IELServices typelib dump (exact method signatures/dispids/params). |
| `evidence_C2a_orlib20_lifecycle.md` | orlib20 init/lookup/delete RE (OrlibInit, FindExistPos, GetMasterRecByName, SetSigDeleted). |
| `evidence_C2b_orlib20_build.md` | orlib20 build/compile/verify/save/error RE (CreateEntryEX, SaveTextEX, VerifyModule_New, SaveModule_New, GetCompilingErrors, FILEDELETE, SigNumToFileName). |
| `re_report.md` | The most complete RE report: orlib20 lifecycle (init/exit), lookup (FindExistPos, GetMasterRecByName, FindExistPosSubType), delete flags (SetSigDeleted/IsSigDeleted), metadata removal (RemoveMetaDataModules/Module), CleanMastersEX, WHType enum + TypeConvert, filename construction (SigNumToFileName, BuildFileName), feasibility answers, master record structure, export address table, remaining unknowns. (Identical content to `evidence_C2a_orlib20_lifecycle.md`; retained under original name for backward compatibility.) |
| `re_output_utf8.txt` | Raw disassembly output (UTF-8), 259 KB. Consult to verify ambiguous claims. |
| `re_output.txt` | Raw disassembly output (original encoding), 256 KB. Consult to verify ambiguous claims. |
| `re_disasm.txt` | Primary per-function disassembly (3,360 lines): OrlibInit, OrlibExit, GetMasterRecByName, FindExistPos, SetSigDeleted, IsSigDeleted, DelMasterRecsBuf, RemoveMetaDataModules, DeleteChangedModulesList, CleanMastersEX, SaveModule_New, GetFileTypeName1, UI delete handler. |
| `re_disasm2.txt` | Targeted functions: delete dispatcher, DeleteItem, FindExistPosSubType, warehouse findByName, GetIdFromRecord. |
| `re_disasm3.txt` | Filename/delete functions: BuildFileName, SetSigDeleted/IsSigDeleted detail, DeleteItem detail, SigNumToFileName. |
| `re_disasm4.txt` | Internal cleanup: deallocator 0x101eae93, RemoveMetaDataModule, DeleteChangedModulesList. |
| `re_disasm5.txt` | Type conversion and warehouse lookup (TypeConvert variants, warehouse singleton accessor). |
| `re_analysis.py` .. `re_analysis5.py` | The pefile+capstone analysis scripts that produced the disasm artifacts. |
| `TlbDump.cs` / `TlbDump.exe` | TypeLib dumper used to extract the ELIS.exe COM interface (IELServices method table, IIDs, ProgID/CLSID/AppID). |

The five `evidence_*.md` names above are historical source names, not persisted
tracked copies: the files are unavailable in the current checkout. The
`re_disasm*.txt`, `re_analysis*.py`, and `TlbDump.cs`/`TlbDump.exe` names likewise
refer to untracked historical artifacts. Current technical status must use the
tracked `docs/RE/`, source, and catalog documents.

**Note on RE method:** The initial RE was performed via static PE analysis (pefile+capstone) because live Frida RE was temporarily unavailable (blocked by a Frida helper-path bug in frida-core 17.15.4 / Luma 1.2.3). **Historical MCP record (superseded):** The study later recorded live RE as available via a Python frida MCP server running Frida 17.17.0 (which included the fix) for follow-up verification and the TVS-2 live-tracing work. That MCP workflow is not current. **Current repository guidance:** use Frida directly from the CLI — `frida-trace`, `frida -l script.js`, or `frida -p PID` — and never Frida MCP, as required by `AGENTS.md`. The static findings above remain valid.
