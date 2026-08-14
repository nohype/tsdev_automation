# TSDev Automation — Reverse-Engineering Feasibility Study

**Status:** Synthesis report (read-only RE phase complete)
**Date:** 2026-08-15
**Scope:** Determine how a standalone C++ CLI can implement three TSDev Library operations (insert / delete / modify, each followed by compile/verify) without using the TSDev.exe GUI, with least custom implementation and least complexity.

---

## 1. Executive Summary

We reverse-engineered the TradeStation Development Environment (TSDev 9.5) to determine how a standalone CLI can insert, delete, and modify EasyLanguage strategies/functions in the TSDev Library and compile/verify them, without the TSDev GUI. Two viable approaches were identified. **Approach 1** drives `ELIS.exe`, the registered COM LocalServer that wraps the library engine (`IELServices` dispatch interface, 7 methods). **Approach 2** loads `orlib20.dll` directly and calls its exported C functions, which are the same primitives TSDev.exe itself uses. Approach 1 is cleaner to call but has structural gaps (no name-to-ProgId resolution, XML-file import rather than raw source, void error reporting on delete/verify, and a likely TradeStation Desktop coupling). Approach 2 exposes every primitive needed (lookup, create, set-source, compile/verify, error retrieval, save, delete) but carries several unconfirmed parameter orders and one GUI-bound save path that requires a workaround. **Recommendation:** adopt Approach 2 (orlib20.dll direct) as the primary path because it is the only approach that can satisfy all three acceptance criteria headless; retain Approach 1 as a fallback for the subset of operations it supports. The highest-ROI next step is to resolve the remaining orlib20 unknowns by RE'ing the caller sites in TSDev.exe and to run a headless `_OrlibInit` smoke test.

---

## 2. Goal & Acceptance Criteria

The project must provide a C++ CLI that performs three operations against the TSDev Library (`C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin`), reliably and consistently, without the TSDev.exe GUI:

1. **Insert** — Given EasyLanguage source code and a name, insert a new strategy/function into the library and compile/verify it. **Fail loudly** if compilation fails.
2. **Delete** — Given the name of an existing strategy/function, delete it from the library. **Notify** if it does not exist.
3. **Modify** — Given the name of an existing strategy/function and new EasyLanguage source, replace its code and recompile/re-verify. **Notify** if it does not exist; **fail loudly** if compilation of the new code fails.

All three must work via a CLI built with the Visual Studio Build Tools. The TradeStation installation is **read-only** and must never be modified.

---

## 3. System Under Study

| Component | Path | Role |
|-----------|------|------|
| TSDev.exe | `Program\TSDev.exe` | 32-bit PE32, 2,178,032 bytes. The GUI host that orchestrates library operations. READ-ONLY. |
| TSDev Library | `MyWork\Bin` | 3,413 `.ell` (compiled/encrypted, magic `RNAM`) + 6,871 `.xml` (symbol/debug metadata, root `<symbols file=... version={GUID}>`). Files named `elmodule_<X>_<N>.ell/.xml` and `tss<hex>.xml`. X in {0,1,2,3,4,1026,1282,4610,5122}. No index/catalog file. *(See `evidence_A_filesystem_survey.md` §E for the MyWork\Bin inventory and `evidence_A2_binary_static_survey.md` §1b/§2a for the exact file counts and the `RNAM` magic-bytes finding.)* |
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

> **Evidence:** The method signatures, dispids, and parameter types below are from the `LoadTypeLibEx` type-library dump in `evidence_C1_elis_typelib.md` §2.5 (IELServices TypeInfo [2]). The COM registration facts (ProgID/CLSID/AppID/TypeLib) are in `evidence_C1_elis_typelib.md` §1; the ELIS.exe string-extracted ProgID/CLSID/method-name strings are corroborated in `evidence_A2_binary_static_survey.md` §5 (ELIS.exe).

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

All findings below are from static PE analysis (pefile + capstone) of `orlib20.dll`; these initial findings remain valid. Live RE was initially blocked by a Frida helper-path bug (frida-core 17.15.4 in Luma 1.2.3); the initial static analysis was performed with pefile+capstone as a read-only substitute. Live RE has since been enabled by switching to a Python frida MCP server running Frida 17.17.0 (which includes the fix). The frida MCP provides `create_interactive_session` + `execute_in_session` giving the full Frida JS API (symbol resolution, disassembly, `Interceptor.attach` for function tracing, `NativeFunction`, memory reads). The tool surface is simpler than Luma's (no radare2/decompile) but sufficient for all RE needs. Evidence is cited by evidence file (`evidence_C2a_orlib20_lifecycle.md` for init/lookup/delete; `evidence_C2b_orlib20_build.md` for build/compile/verify/save/error), `re_report.md` section, and disasm file/line ranges. All cited files are in `tmp\re_artifacts\` (see §9 Evidence Index).

### 5.1 Primitive table

| Primitive | Signature | Confidence | Evidence |
|-----------|-----------|------------|----------|
| Init | `BOOL __stdcall _OrlibInit@8(const char* workDirPath, int contextId)` | MED | `evidence_C2a_orlib20_lifecycle.md` §2.1; re_report 2.1; `re_disasm.txt` L5-139. Ref-counted at `0x1023acb0`. `workDirPath` is a writable path (EnsureDirectoryExists), NOT an HINSTANCE. `contextId` passed to `ELRte::CreateRte`. |
| Teardown | `BOOL __stdcall _OrlibExit@0(void)` | HIGH | `evidence_C2a_orlib20_lifecycle.md` §2.2; re_report 2.2; `re_disasm.txt` L227-275. Decrements refcount; tears down at zero via `ELRte::DestroyRte`. |
| Name->ID lookup | `int __cdecl FindExistPos(const char* name, int* WHType) -> int` | HIGH | `evidence_C2a_orlib20_lifecycle.md` §2.4; re_report 2.4; `re_disasm.txt` L675-737. Returns ProgId/N >=0 on success, -1 if absent. ID = record[+0x18]. `*WHType` is an in/out (initialized to 0, updated by lookup). |
| Subtype lookup | `int __stdcall _FindExistPosSubType@12(const char* name, int WHType, int* outSubType) -> int` | MED | `evidence_C2a_orlib20_lifecycle.md` §2.5; re_report 2.5; `re_disasm2.txt` L342-384. |
| Get master record | `BOOL __stdcall _GetMasterRecByName@12(const char* name, int WHType, void* outRecord)` | HIGH | `evidence_C2a_orlib20_lifecycle.md` §2.3; re_report 2.3; `re_disasm.txt` L451-484. Copies 268-byte (0x42 dwords + 1 word) master record. |
| Create entry | `int __stdcall _CreateEntryEX@24(const char* name, ?, ?, ?, ?, int* outErrCode) -> int` | PARTIAL | `evidence_C2b_orlib20_build.md` §3 (`_CreateEntryEX@24` disasm). Returns new ProgId or -1. `*outErrCode` = 0xBFFE (alloc-fail) / 0xBFFD (create-failed/name-conflict). **Confirmed:** param1=name, param6=outErrCode. **Unknown:** params 2-5 (WHType + flags among them, exact order unconfirmed). |
| Set source text (non-GUI) | `void* _SaveTextEX@16(void* p1, const char* sourceTextAnsi, const char* name, void* p4)` | MED | `evidence_C2b_orlib20_build.md` §9 (`_SaveTextEX@16` disasm). p2 = source text (HIGH; lstrlenA'd -> warehouse.SetSource). p3 = name (MED). p1 nonzero-checked + lstrlenA'd (uncertain). p4 unused in visible path (uncertain). |
| Start compile | `void StartCompilingProcess(void)` (cdecl, 0 params) | HIGH | `evidence_C2b_orlib20_build.md` §1 (`StartCompilingProcess` disasm). Resets error array `[0x1023cea0]` + count `[0x1023cea4]`; record stride 0x129 (297); allocs compiler state `[0x1023ceb0]`. |
| End compile | `void EndCompilingProcess(void)` (0 params) | HIGH | `evidence_C2b_orlib20_build.md` §2 (`EndCompilingProcess` disasm). Tears down compiler state. |
| Verify module | `BOOL __stdcall _VerifyModule_New@20(HWND parent, int WHType, void* sourceOrModule, ?, ?)` | PARTIAL | `evidence_C2b_orlib20_build.md` §5 (`_VerifyModule_New@20` disasm). param1=HWND, param2=WHType (HIGH); param3=src/module (MED); p4/p5 unknown. |
| Verify by name (wrapper) | `BOOL VerifyATechnique(const char* name, int WHType) -> BOOL` | HIGH | `evidence_C2b_orlib20_build.md` §5 (`VerifyATechnique` wrapper disasm). Looks up module internally; preferred for CLI. |
| Get errors | `std::string _GetCompilingErrors@4(std::string* hiddenRetPtr)` | MED-HIGH | `evidence_C2b_orlib20_build.md` §7 (`_GetCompilingErrors@4` disasm). Builds concatenated "name: msg\n..." across all error records; if array empty, reads `VERIFY.ERR`. |
| Get error level | `int _GetErrorLevel@4(int idx) -> int` | HIGH | `evidence_C2b_orlib20_build.md` §8 (`_GetErrorLevel@4` disasm). 1 if record[0]==0x7775 sentinel (error), 0 = warning/none. Error count = global `[0x1023cea4]`. No single "overall level"; caller checks count>0 + per-record levels. |
| Persist/save | `BOOL __stdcall _SaveModule_New@20(int WHType, const char* name, ?, HWND editorHwnd, ?)` | HIGH (GUI-bound) | `evidence_C2b_orlib20_build.md` §6 (`_SaveModule_New@20` disasm). GUI-bound via its inner callee 0x10159770 (IsWindow check) → SaveToDisk 0x101081c0 which reads source via WM_GETTEXT from an edit control, converts wide→UTF-8, calls warehouse.SetSourceText then `MDVault::SaveModule` (writes `elmodule_<X>_<N>.ell/.xml`); also shows a QuickEditor→PowerEditor MessageBox warning. See `evidence_C2b_orlib20_build.md` §6. |
| Mark deleted (flag only) | `void __stdcall _SetSigDeleted@4(void* masterRecord)` | HIGH | `evidence_C2a_orlib20_lifecycle.md` §2.6; `evidence_C2b_orlib20_build.md` §10; re_report 2.6; `re_disasm.txt` L1127-1141. Sets byte@0x79 bit0, clears word@0xD8 bit1. NO file I/O. |
| Is deleted | `BOOL __stdcall _IsSigDeleted@4(const void* rec)` | HIGH | `evidence_C2a_orlib20_lifecycle.md` §2.7; `evidence_C2b_orlib20_build.md` §10; re_report 2.7; `re_disasm.txt` L1347-1361. |
| File removal | `void FILEDELETE(void* moduleInfo, ?, LPCWSTR path)` (export) | MED | `evidence_C2b_orlib20_build.md` §11 (`FILEDELETE` disasm) + DeleteFile xref map. DeleteFileW + dependent files. |
| Path builder | `void SigNumToFileName(char* outBuf, int WHType, int progId)` | HIGH | `evidence_C2b_orlib20_build.md` (`SigNumToFileName` disasm); `evidence_C2a_orlib20_lifecycle.md` §4.4; re_report 4.4; `re_disasm3.txt`. Result: `<basepath[WHType]><letter><NNNN>` where letter = S/F/I/B/M for WHType 0/1/2/3/4, NNNN = progId zero-padded-4. Base-path table at `0x10243000`. |
| Remove metadata (chained) | `void RemoveMetaDataModules(const char* name, int sigNum)` | MED | `evidence_C2a_orlib20_lifecycle.md` §3.2; re_report 3.2; `re_disasm.txt` L1795-1829. Calls `MDVault::RemoveChainedModules`. Does NOT directly delete files. |
| Remove metadata (single) | `void RemoveMetaDataModule(const char* name)` (unexported) | MED | `evidence_C2a_orlib20_lifecycle.md` §3.3; re_report 3.3; `re_disasm4.txt` L207-296. Calls `MDVault::RemoveModule`. |
| Clean masters | `BOOL __stdcall _CleanMastersEX@4(int WHType)` | MED | `evidence_C2a_orlib20_lifecycle.md` §3.6; re_report 3.6; `re_disasm.txt` L2691-2796. Iterates records, clears changed/linked flag @0xe4. |
| Warehouse singleton | `void* GetWarehouse(const char* nameOrZero)` (Ord 10 @ 0x101A9650) | HIGH | `evidence_C2b_orlib20_build.md` (`GetWarehouse singleton` disasm); `evidence_C2a_orlib20_lifecycle.md` §2.3. Thread-safe; called by nearly every function. |
| Direct save (alt) | `MDVault::SaveModule(ModuleInfo*, std::wstring&...)` (ELMDCore.dll, mangled) | LOW (unconfirmed) | `evidence_C2b_orlib20_build.md` §6 (mangled import symbol referenced in `_SaveModule_New` semantics). Bypasses the GUI-bound `_SaveModule_New`. |

### 5.2 WHType enum

| WHType | Name | Letter | Subtypes |
|--------|------|--------|----------|
| 0 | Strategy | S | 0,1 = Strategy; 2,3 = Strategy Group |
| 1 | Function | F | (single) |
| 2 | Indicator | I | 0,1 = Indicator; 2,3 = PaintBar; 4,5 = ShowMe; 6,7 = Expert; 8,9 = Expiration Rule; 0xa,0xb = Volatility Model; 0xc,0xd = Pricing Model; 0xe,0xf = Bid/Ask Model; 0x10,0x11 = Search Strategy; 0x12,0x13 = ProbabilityMap |
| 3 | ActivityBar | B | (single) |
| 4 | Function (Read Only) | M | (single) |
| 0x32 (50) | Unknown (composite/project?) | — | Appears in verify/save; meaning unknown. |

TypeConvert (`0x101d43a0`) expands a WHType into a composite-ID list (subtype<<8 | WHType) terminated by -1, used by warehouse lookups to search all subtypes.

### 5.3 Proposed call sequences (pseudocode)

**Common preamble:**
```cpp
SetDllDirectory("C:\\Program Files (x86)\\TradeStation 9.5\\Program");
BOOL ok = _OrlibInit(workDirPath, contextId);   // contextId TBD by smoke test
if (!ok) { /* fail loudly */ }
```

**INSERT:**
```cpp
int errCode = 0;
int progId = _CreateEntryEX(name, /*WHType+flags: UNCONFIRMED order*/, &errCode);
if (progId < 0) {
    // errCode 0xBFFE = alloc fail; 0xBFFD = create fail / name conflict
    fail_loudly("insert failed: 0x%X", errCode);
}
_SaveTextEX(/*p1: UNCONFIRMED*/, sourceTextAnsi, name, /*p4: UNCONFIRMED*/);
StartCompilingProcess();
BOOL vok = VerifyATechnique(name, whType);   // preferred over _VerifyModule_New
EndCompilingProcess();
int errCount = *(int*)0x1023cea4;            // error record count
if (errCount > 0) {
    std::string errs = _GetCompilingErrors(&retPtr);
    for (int i=0;i<errCount;i++) if (_GetErrorLevel(i)==1) { fail_loudly(errs); }
}
// Persist: GUI-bound path -> workaround
HWND hEdit = CreateWindowW("EDIT", ..., WS_POPUP, 0,0,0,0, NULL, NULL, hInst, NULL);
SetWindowTextW(hEdit, widen(sourceText));
_SaveModule_New(whType, name, /*p3*/, hEdit, /*p5*/);
DestroyWindow(hEdit);
_OrlibExit();
```

**MODIFY:**
```cpp
int whType = expectedType;
int progId = FindExistPos(name, &whType);
if (progId < 0) { notify_not_found(name); return; }
_SaveTextEX(/*p1*/, newSourceAnsi, name, /*p4*/);
StartCompilingProcess();
VerifyATechnique(name, whType);
EndCompilingProcess();
// check errors as above; fail loudly on error level==1
// persist via hidden-EDIT workaround (same as INSERT)
```

**DELETE:**
```cpp
int whType = expectedType;
int progId = FindExistPos(name, &whType);
if (progId < 0) { notify_not_found(name); return; }
char masterRec[268];
_GetMasterRecByName(name, whType, masterRec);
_SetSigDeleted(masterRec);
// persist master index (save mechanism TBD — see unknowns)
RemoveMetaDataModules(name, progId);
// file removal:
char path[MAX_PATH]; SigNumToFileName(path, whType, progId);
FILEDELETE(/*moduleInfo*/, /*p2*/, widen(path));   // deletes .ell + dependents
_CleanMastersEX(whType);
_OrlibExit();
```

### 5.4 GUI-save workaround

`_SaveModule_New@20` is GUI-bound: it reads source via `WM_GETTEXT` from an `IsWindow`-checked EDIT control, converts wide->UTF-8, sets warehouse source, then calls `MDVault::SaveModule` (which writes `elmodule_<X>_<N>.ell/.xml`). It also shows a QuickEditor->PowerEditor MessageBox warning.

**Workaround (low complexity):** Create a hidden `EDIT` control (`WS_POPUP`, zero-size), `SetWindowTextW` it with the source, and pass its HWND as `editorHwnd`. This satisfies the `IsWindow` + `WM_GETTEXT` path without a visible UI. The MessageBox warning may still fire; suppress by either pre-configuring the "don't warn" state (if a registry/config flag exists) or by auto-dismissing via a thread hook.

**Alternative (unconfirmed):** Call `MDVault::SaveModule` directly (ELMDCore.dll import, mangled `?SaveModule@MDVault@ELMeta@@QAE_NPAVModuleInfo@2@AAV?$basic_string@_W...`). This bypasses the GUI entirely but the exact `ModuleInfo` construction and parameter list are unconfirmed.

### 5.5 Remaining unknowns / blockers

1. **`_SaveModule_New` GUI dependency** (highest blocker) — resolvable via hidden EDIT control workaround (low complexity) or `MDVault::SaveModule` direct (unconfirmed).
2. **`_CreateEntryEX` params 2-5** (WHType position) — unconfirmed.
3. **`_SaveTextEX` p1/p4 roles** — uncertain.
4. **`_VerifyModule_New` p3 type + p4/p5** — uncertain (mitigated by using `VerifyATechnique` wrapper instead).
5. **Whether `_SaveTextEX` alone persists or save is mandatory** — unconfirmed.
6. **`_GetCompilingErrors` exact formatting** — MED-HIGH confidence.
7. **Error record field layout** (stride 297) beyond the 0x7775 sentinel — unmapped.
8. **WHType 0x32 special case** — unknown.
9. **Which function directly deletes `.ell` files** — `DeleteFileA/W` is imported; `FILEDELETE` is the export but the exact orchestration (master-index save between SetSigDeleted and FILEDELETE) is inferred from TSDev.exe behavior, not fully traced in orlib20.
10. **Valid `contextId` values for `_OrlibInit`** — passed to `ELRte::CreateRte`; needs runtime testing.

---

## 6. Comparison & Recommendation

| Criterion | Approach 1 (ELIS.exe COM) | Approach 2 (orlib20.dll direct) |
|-----------|---------------------------|---------------------------------|
| Name -> (WHType, ProgId) resolution | NOT exposed; must scan XML out-of-band | `FindExistPos` / `_GetMasterRecByName` (HIGH conf) |
| Raw-source insert | No — XML file import only | `_CreateEntryEX` + `_SaveTextEX` (MED conf) |
| Compile/verify | `VerifyAll` (whole library, void) | `StartCompilingProcess` + `VerifyATechnique` (HIGH conf) |
| Error reporting | Void on delete/verify; results BSTR on import only | `_GetCompilingErrors` + `_GetErrorLevel` + count global (MED-HIGH conf) |
| Delete | `DeleteAnalysisTechnique(WHType,ProgId)` void; needs ProgId | `SetSigDeleted` + `RemoveMetaDataModules` + `FILEDELETE` + `SigNumToFileName` (HIGH conf on pieces, MED on orchestration) |
| Modify | No primitive (delete+insert) | Set source + verify + save (same primitives as insert) |
| Complexity / risk | Low call complexity, HIGH gap-filling complexity (XML synth, name resolution, error inference) | MED call complexity, MED param-order risk (resolvable via caller RE) |
| Headless viability | Uncertain (Desktop coupling suspected; void errors) | Likely YES (no TSDev/TS import; needs CreateRte smoke test) |

### Recommendation

**Primary: Approach 2 (orlib20.dll direct invocation).** It is the only approach that exposes primitives for all three acceptance criteria, including targeted compile/verify with retrievable errors and name resolution. Its risks are concentrated in a small number of unconfirmed parameter orders and one GUI-bound save path, all of which have identified resolution paths. It aligns with the project goal of least custom implementation: the CLI becomes a thin orchestrator over the same engine TSDev.exe uses.

**Fallback: Approach 1 (ELIS.exe COM).** Retain for the delete operation only, and only if name resolution is handled externally (e.g., XML scan) and the void return is acceptable. Not viable for insert/modify due to XML-only import and absent error reporting.

The recommendation is conditional on two de-risking gates (see Next Steps): (a) resolving the orlib20 unknown param orders, and (b) confirming `_OrlibInit` + `ELRte::CreateRte` succeeds without TradeStation Desktop running.

---

## 7. Next Steps (TVS-2 Proposal)

The highest-ROI next slice de-risks the recommendation before any broad implementation:

**a) Resolve orlib20 unknown parameter orders (RE).**
Primary approach — **live tracing via the frida MCP** (Frida 17.17.0): spawn or attach to TSDev.exe via `create_interactive_session`, hook the orlib20 functions (`_CreateEntryEX@24`, `_SaveTextEX@16`, `_SaveModule_New@20`, `_VerifyModule_New@20`) with `Interceptor.attach` to capture `args[0..N]` in `onEnter`, then trigger insert/delete/modify operations in the TSDev UI to observe the actual argument values passed by the authoritative caller. This gives dynamic, authoritative argument values and is more reliable than static disassembly.

Secondary/confirmatory approach — **RE the caller sites in TSDev.exe** (static, deterministic): disassemble the call sites of the same functions within TSDev.exe to recover the exact argument order and types the GUI host uses. Use this to corroborate the live-trace findings.

As a quick preliminary to the headless smoke test (step b), hook `_OrlibInit@8` in ELIS.exe at startup via the frida MCP to capture its actual arguments (`workDirPath`, `contextId`) — this resolves open question #2 (`contextId` valid values) directly from the authoritative caller.

**b) Headless init smoke test (critical gate).**
Build a minimal 32-bit C++ test that loads orlib20.dll, sets the DLL search directory to the TS Program folder, and calls `_OrlibInit(workDirPath, contextId)` with a candidate `contextId` (try 0 and small integers). Assert the return is nonzero and that `ELRte::CreateRte` succeeded (observable via a subsequent `FindExistPos` call returning a known existing name). Run this **without** TradeStation Desktop running. This is the critical headless-viability gate. If it fails, the recommendation must be revisited.

**c) Implement the CLI with TDD (red-green), starting from DELETE.**
Order by ascending risk:
1. **DELETE** (simplest, lowest risk): `FindExistPos` -> notify if -1 -> `_GetMasterRecByName` -> `_SetSigDeleted` -> persist -> `RemoveMetaDataModules` -> `FILEDELETE`. Write the failing test first (delete a known throwaway strategy), then implement to green.
2. **INSERT**: `_CreateEntryEX` -> `_SaveTextEX` -> compile/verify -> error check -> save (hidden-EDIT workaround).
3. **MODIFY**: `FindExistPos` -> notify if -1 -> `_SaveTextEX` -> compile/verify -> error check -> save.

Each function gets a red-green TDD cycle against a disposable test library copy (never the real install).

---

## 8. Open Questions / Risks

1. **Headless `CreateRte`**: Does `ELRte::CreateRte` succeed without TradeStation Desktop? (Critical gate; unknown until smoke test.)
2. **`contextId` valid values**: What integer(s) are valid for `_OrlibInit`'s second argument? (Needs runtime test.)
3. **`_CreateEntryEX` params 2-5**: Exact order/meaning of WHType and flags. (Resolve via TSDev.exe caller RE.)
4. **`_SaveTextEX` p1/p4**: Roles of param 1 (nonzero-checked, lstrlenA'd) and param 4 (unused in visible path). (Resolve via caller RE.)
5. **`_SaveModule_New` GUI binding**: Does the hidden-EDIT workaround fully suppress the MessageBox? Is there a registry/config flag to disable the QuickEditor->PowerEditor warning?
6. **`MDVault::SaveModule` direct**: Feasible as a GUI-free alternative? Needs `ModuleInfo` construction details.
7. **Persistence after `_SetSigDeleted`**: Which call persists the master index with the deleted flag before `FILEDELETE` removes files? (Inferred from TSDev.exe orchestration; not fully traced.)
8. **`_GetCompilingErrors` formatting**: Exact string format for error reporting (MED-HIGH conf).
9. **Error record layout** (stride 297): Fields beyond the 0x7775 sentinel are unmapped.
10. **WHType 0x32**: Meaning unknown; may affect verify/save.
11. **`_SaveTextEX` persistence**: Does it persist alone, or is `_SaveModule_New` always mandatory?
12. **Registry dependence**: orlib20 uses `orshr20.dll!_TSGetRegistryRootKeyA` and ADVAPI32 registry functions; the TS install's registry keys must be present.
13. **Frida live RE**: Live RE was initially blocked by a Frida helper-path bug (frida-core 17.15.4 in Luma 1.2.3); static analysis was used instead and is equivalent for signatures. Live RE has since been enabled by switching to a Python frida MCP server running Frida 17.17.0 (which includes the fix), providing the full Frida JS API via `execute_in_session` for runtime value confirmation and the TVS-2 live-tracing work.

---

## 9. Evidence Index

All artifacts in `C:\Users\user\projects\tsdev_automation\tmp\re_artifacts\`:

| File | Contents |
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

The five `evidence_*.md` files are the persisted, citable copies of the subagent investigation reports that underpin the technical claims in this document. The `re_disasm*.txt` files are the raw capstone disassembly per function; the `re_analysis*.py` files are the static PE analysis scripts; `TlbDump.cs`/`TlbDump.exe` is the typelib dumper tool used for the C1 evidence.

**Note on RE method:** The initial RE was performed via static PE analysis (pefile+capstone) because live Frida RE was temporarily unavailable (blocked by a Frida helper-path bug in frida-core 17.15.4 / Luma 1.2.3). Live RE is now available via the frida MCP (Python frida MCP server running Frida 17.17.0, which includes the fix) for follow-up verification and the TVS-2 live-tracing work. The static findings above remain valid.
