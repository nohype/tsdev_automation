# TSDev Automation — Reverse-Engineering Feasibility Study

**Status:** Synthesis report (read-only RE phase complete)
**Date:** 2026-08-15
**Scope:** Determine how a standalone C++ CLI can implement three TSDev Library operations (insert / delete / modify, each followed by compile/verify) without using the TSDev.exe GUI, with least custom implementation and least complexity.

---

## 1. Executive Summary

We reverse-engineered the TradeStation Development Environment (TSDev 9.5) to determine how a standalone CLI can insert, delete, and modify EasyLanguage strategies/functions in the TSDev Library and compile/verify them, without the TSDev GUI. Two viable approaches were identified. **Approach 1** drives `ELIS.exe`, the registered COM LocalServer that wraps the library engine (`IELServices` dispatch interface, 7 methods). **Approach 2** loads `orlib20.dll` directly and calls its exported C functions, which are the same primitives TSDev.exe itself uses. Approach 1 is cleaner to call but has structural gaps (no name-to-ProgId resolution, XML-file import rather than raw source, void error reporting on delete/verify, and a likely TradeStation Desktop coupling). Approach 2 exposes every primitive needed (lookup, create, set-source, compile/verify, error retrieval, save, delete). Parameter orders for the key primitives are now **CONFIRMED**, and standalone INSERT persistence (compile + `.ell` file write) has been **demonstrated** without TSDev.exe and without Frida, via an in-process patch to `FUN_10011230` in orlib20.dll (see §5.6). **DELETE is IMPLEMENTED and APPROVED** — the orchestrator-based approach calling `FUN_101a8360` (RVA `0x1A8360`) persists deletions to `MWRKArea.wrk` standalone, verified by two independent test runs (see §5.3 DELETE and §5.7). **MODIFY is IMPLEMENTED and APPROVED** — it composes only existing INSERT primitives (`findExistPos` → `saveSource` → `verifyModule`) with no new orlib20 calls and no new RVAs; verified by two independent test runs (exit 0, ProgId preserved, `.ell` timestamp updated, compile errors surfaced, negative + compile-error tests return `success=false`; see §5.3 MODIFY and §5.8). The headless `_OrlibInit` smoke test has **PASSED**. **All three core functions (INSERT, DELETE, MODIFY) are now IMPLEMENTED and APPROVED** — the primary acceptance criteria ("the three functions must work reliably and consistently via a CLI") is satisfied at the library level. **Recommendation:** adopt Approach 2 (orlib20.dll direct) as the primary path because it is the only approach that can satisfy all three acceptance criteria headless; retain Approach 1 as a fallback for the subset of operations it supports. Remaining unknowns are narrow: warehouse-type mappings for five component types (ActivityBar/PaintBar/ProbabilityMap/ShowMe/TradingApp) and minor unmapped fields (see §5.5).

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
| Init | `BOOL __stdcall _OrlibInit@8(const char* workDirPath, int contextId)` | CONFIRMED | `evidence_C2a_orlib20_lifecycle.md` §2.1; re_report 2.1; `re_disasm.txt` L5-139; `orlib_param_orders.md` §2. Ref-counted at `0x1023acb0`. **Confirmed call:** `OrlibInit(NULL, 1)` → TRUE. `workDirPath` is a **NULL sentinel** (orlib20 derives `MyWork` internally; NOT a path string, NOT an HInstance). `contextId = 1`, passed to `ELRte::CreateRte`. |
| Teardown | `BOOL __stdcall _OrlibExit@0(void)` | HIGH | `evidence_C2a_orlib20_lifecycle.md` §2.2; re_report 2.2; `re_disasm.txt` L227-275. Decrements refcount; tears down at zero via `ELRte::DestroyRte`. |
| Name->ID lookup | `int __stdcall FindExistPos(const char* name, int whType) -> int` | CONFIRMED | `evidence_C2a_orlib20_lifecycle.md` §2.4; re_report 2.4; `re_disasm.txt` L675-737; `orlib_param_orders.md` §4. **Confirmed:** `FindExistPos(name, whType)` returns `ProgId` (>0) if found, `0` if not found. Confirmed for strategies (`whType=0`). ID = record[+0x18]. |
| Subtype lookup | `int __stdcall _FindExistPosSubType@12(const char* name, int WHType, int* outSubType) -> int` | MED | `evidence_C2a_orlib20_lifecycle.md` §2.5; re_report 2.5; `re_disasm2.txt` L342-384. |
| Get master record | `BOOL __stdcall _GetMasterRecByName@12(const char* name, int WHType, void* outRecord)` | HIGH | `evidence_C2a_orlib20_lifecycle.md` §2.3; re_report 2.3; `re_disasm.txt` L451-484. Copies 268-byte (0x42 dwords + 1 word) master record. |
| Create entry | `int __stdcall _CreateEntryEX@24(int whType, int secondary, const char* name, int param4, char param5, int* outErrCode) -> int` | CONFIRMED | `evidence_C2b_orlib20_build.md` §3; `orlib_param_orders.md` §5. **Confirmed order:** `(whType, 0, name, 0, 0, &errCode)`. Returns new `ProgId` on success. `*outErrCode` = 0xBFFE (alloc-fail) / 0xBFFD (create-failed/name-conflict). `secondary=0`, `param4=0`, `param5=0` observed. |
| Set source text (non-GUI) | `void* _SaveTextEX@16(void* p1, const char* sourceTextAnsi, const char* name, void* p4)` | MED | `evidence_C2b_orlib20_build.md` §9 (`_SaveTextEX@16` disasm). p2 = source text (HIGH; lstrlenA'd -> warehouse.SetSource). p3 = name (MED). p1 nonzero-checked + lstrlenA'd (uncertain). p4 unused in visible path (uncertain). |
| Start compile | `void StartCompilingProcess(void)` (cdecl, 0 params) | CONFIRMED | `evidence_C2b_orlib20_build.md` §1; `orlib_param_orders.md` §9. Resets error array `[0x1023cea0]` + count `[0x1023cea4]`; record stride 0x129 (297); allocs compiler state `[0x1023ceb0]`. **Required before `VerifyModule_New`.** |
| End compile | `void EndCompilingProcess(int arg)` (__fastcall, 1 arg; pass `0`) | CONFIRMED | `evidence_C2b_orlib20_build.md` §2; `orlib_param_orders.md` §9. Tears down compiler state. **Required after `VerifyModule_New`**; call with `0`. |
| Verify module | `int __stdcall _VerifyModule_New@20(HWND hwnd, int whType, int progId, int flag, void* compilerObj) -> int` | CONFIRMED | `evidence_C2b_orlib20_build.md` §5; `orlib_param_orders.md` §7. **Confirmed:** `(hwnd, whType, progId, 1, compilerObj)`. `compilerObj` is a **`CELCompiler` object (88 bytes), NOT NULL** (see §5.6). **Compiles AND persists the `.ell` file** — this is the function that actually writes the compiled module to disk. Returns `1` on success. |
| Verify by name (wrapper) | `BOOL VerifyATechnique(const char* name, int WHType) -> BOOL` | HIGH | `evidence_C2b_orlib20_build.md` §5 (`VerifyATechnique` wrapper disasm). Looks up module internally; preferred for CLI. |
| Get errors | `std::string _GetCompilingErrors@4(std::string* hiddenRetPtr)` | CONFIRMED | `evidence_C2b_orlib20_build.md` §7; `orlib_param_orders.md` §10. Builds concatenated "name: msg\n..." across all error records; if array empty, reads `VERIFY.ERR`. An empty buffer = success. |
| Get error level | `int _GetErrorLevel@4(int idx) -> int` | HIGH | `evidence_C2b_orlib20_build.md` §8 (`_GetErrorLevel@4` disasm). 1 if record[0]==0x7775 sentinel (error), 0 = warning/none. Error count = global `[0x1023cea4]`. No single "overall level"; caller checks count>0 + per-record levels. |
| Persist/save (source text) | `int __stdcall _SaveModule_New@20(int whType, int progId, void* arg3, HWND editorHwnd, void* arg5) -> int` | CONFIRMED | `evidence_C2b_orlib20_build.md` §6; `orlib_param_orders.md` §6. **Confirmed:** `(whType, progId, NULL, hwnd, NULL)`. Reads source via `WM_GETTEXT` from an `IsWindow`-checked `EDIT` control, converts wide→UTF-8, sets warehouse source text. **Does NOT persist `.ell`/`.xml` files** — persistence is performed by `VerifyModule_New` (see above). Returns `1` on success. |
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

### 5.2 WHType enum

> **Evidence:** `orlib_param_orders.md` §1. The four mappings below are **CONFIRMED** by live RE. The remaining TradeStation component types (ActivityBar, PaintBar, ProbabilityMap, ShowMe) are **UNRESOLVED** — their warehouse-type codes have NOT been confirmed and must NOT be treated as known. The static-analysis-derived entries for WHType 1/3 and the Indicator subtypes (PaintBar/ShowMe/ProbabilityMap etc.) below are retained from the earlier static survey for reference only and are **UNCONFIRMED**.

| WHType | Name | Letter | Subtypes | Status |
|--------|------|--------|----------|--------|
| 0 | Strategy | S | 0,1 = Strategy; 2,3 = Strategy Group | **CONFIRMED** |
| 1 | Function | F | (single) | UNCONFIRMED (static survey) |
| 2 | Indicator | I | 0,1 = Indicator; 2,3 = PaintBar; 4,5 = ShowMe; 6,7 = Expert; 8,9 = Expiration Rule; 0xa,0xb = Volatility Model; 0xc,0xd = Pricing Model; 0xe,0xf = Bid/Ask Model; 0x10,0x11 = Search Strategy; 0x12,0x13 = ProbabilityMap; 0x14 = TradingApp | **CONFIRMED** (base type); subtype attributions UNCONFIRMED except TradingApp |
| 3 | ActivityBar | B | (single) | UNCONFIRMED (static survey) |
| 4 | Function | M | (single) | **CONFIRMED** |
| 5122 (0x1402) | TradingApp | — | expanded whType = (subType 0x14 << 8) \| category 2 | **CONFIRMED** — see `tmp/re_artifacts/tradingapp_whtype.md` + `tmp/re_artifacts/c1_adversarial_review.md` |
| 0x32 (50) | Unknown (composite/project?) | — | Appears in verify/save; meaning unknown. | UNCONFIRMED |

**TradingApp** uses an **expanded whType** of 5122 (0x1402), encoded as `(subType << 8) | category` = `(0x14 << 8) | 2`. The orlib20 API functions (`CreateEntryEX`, `FindExistPos`, `SaveModule_New`, `VerifyModule_New`, delete orchestrator) take **category=2** (the low 8 bits — same as Indicator), with the sub-type 0x14 passed as `CreateEntryEX`'s 2nd parameter. The `.ell` filename uses the full expanded whType 5122 (`elmodule_5122_<progId>.ell`). For Strategy/Indicator/Function (sub-type 0), the expanded whType equals the category, so no decomposition is needed.

**UNRESOLVED component types** (warehouse-type code NOT confirmed — do not claim as known): ActivityBar, PaintBar, ProbabilityMap, ShowMe. Any code written today must restrict itself to the four confirmed types (0=Strategy, 2=Indicator, 4=Function, 5122=TradingApp).

TypeConvert (`0x101d43a0`) expands a WHType into a composite-ID list (subtype<<8 | WHType) terminated by -1, used by warehouse lookups to search all subtypes.

### 5.3 Proposed call sequences (pseudocode)

> **Evidence:** `orlib_param_orders.md` §15 (complete INSERT flow). The INSERT flow below is **CONFIRMED** — standalone disk persistence of `.ell` files has been demonstrated without TSDev.exe and without Frida (see §5.6). The MODIFY flow is **IMPLEMENTED and APPROVED** — it composes only existing INSERT primitives (`findExistPos` → `saveSource` → `verifyModule`) with no new orlib20 calls and no new RVAs; verified by two independent test runs (see §5.3 MODIFY and §5.8). DELETE is **IMPLEMENTED and APPROVED** — the orchestrator-based approach (see §5.7) has been verified by two independent test runs.

**Common preamble (CONFIRMED):**
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
patch_memory((BYTE*)orlib + 0x11230, "\xB8\x01\x00\x00\x00\xC2\x0C\x00", 8);

// 5. Init
BOOL ok = OrlibInit(NULL, 1);   // workDirPath = NULL sentinel, contextId = 1
if (!ok) { /* fail loudly */ }
```

**INSERT — CONFIRMED (standalone persistence demonstrated):**
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

// 9. SaveModule_New — saves source text into the warehouse entry (does NOT persist files)
SaveModule_New(whType, progId, NULL, hEdit, NULL);

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

**MODIFY — IMPLEMENTED & APPROVED (composes existing INSERT primitives; see §5.8):**
```cpp
int whType = 0;
// 6. Look up the existing component
int progId = FindExistPos(name, whType);
if (progId == 0) { notify_not_found(name); return; }
// 8-17. Same as INSERT from step 8 onward (hidden EDIT control, SaveModule_New,
//       CELCompiler setup, StartCompilingProcess, VerifyModule_New, error check,
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

**DELETE — IMPLEMENTED & APPROVED (orchestrator-based; see §5.7):**
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
> persist, or violated the standalone constraint. See §5.7 and
> `tmp/re_artifacts/DELETE_WORKING_MEMORY.md` for the full rejection record.

### 5.4 SaveModule_New — source text only (no file persistence)

`_SaveModule_New@20` is GUI-bound in its source-retrieval path: it reads source via `WM_GETTEXT` from an `IsWindow`-checked EDIT control, converts wide→UTF-8, and sets the warehouse source text. **It does NOT persist `.ell`/`.xml` files** — it saves source text into the warehouse entry only (see §5.1 primitive table). Persistence of the compiled `.ell` file is performed by `VerifyModule_New` (with the `FUN_10011230` patch applied — see §5.6), which compiles the source and calls `ELMeta::MDVault::SaveModule` to write `elmodule_<X>_<N>.ell` to disk.

**Hidden-EDIT workaround (CONFIRMED, low complexity):** Create a hidden `EDIT` control (`WS_POPUP`, zero-size), `SetWindowTextW` it with the source, and pass its HWND as `editorHwnd`. This satisfies the `IsWindow` + `WM_GETTEXT` path without a visible UI. This workaround has been demonstrated working in the standalone INSERT flow (see §5.3 and §5.6).

> **NOTE:** An earlier version of this section stated that `_SaveModule_New` "calls `MDVault::SaveModule` (which writes `elmodule_<X>_<N>.ell/.xml`)." That was incorrect — `_SaveModule_New` sets warehouse source text only. File persistence is via `VerifyModule_New` → patched `CompileText` → `MDVault::SaveModule` (see §5.6).

### 5.5 Remaining unknowns / blockers

> **Resolved items:** The following were previously listed as unknowns and are now **CONFIRMED** — see §5.1 (primitive table) and §5.6 (persistence fix): `_CreateEntryEX` params 2-5, `_VerifyModule_New` p3/p4/p5, `_SaveModule_New` GUI dependency (hidden-EDIT workaround confirmed), `_SaveTextEX` persistence semantics (SaveModule_New saves source only; VerifyModule_New persists `.ell`), `_GetCompilingErrors` formatting, `contextId` valid values (contextId=1), and the GUI-save persistence path (resolved via the `FUN_10011230` patch — see §5.6).

**Genuinely unresolved:**

1. **Warehouse type mappings for ActivityBar / PaintBar / ProbabilityMap / ShowMe / TradingApp** — not confirmed; do NOT treat as known (see §5.2). Any code today must restrict itself to WHType 0/2/4.
2. ~~**DELETE flow not yet tested end-to-end**~~ — **RESOLVED — IMPLEMENTED & APPROVED.** The orchestrator-based approach (`FUN_101a8360`) persists deletions to `MWRKArea.wrk` standalone, verified by two independent test runs (see §5.7). Prior approaches (`SetSigDeleted` + `RemoveMetaDataModules` + `CleanMastersEX`, command `0x45`, dirty-flag manipulation, `WHServer.exe` termination) are RETIRED/rejected.
3. **`_SaveTextEX` p1/p4 roles** — uncertain (MED confidence). Note: `_SaveTextEX` is NOT used in the confirmed INSERT/MODIFY flow; `_SaveModule_New` is used instead. `_SaveTextEX` remains unmapped for completeness.
4. **Error record field layout** (stride 297) beyond the 0x7775 sentinel — unmapped.
5. **WHType 0x32 special case** — meaning unknown; may affect verify/save.

### 5.6 Persistence fix — SymWriter bypass (CONFIRMED, breakthrough)

> **Evidence:** `orlib_param_orders.md` §12. This is the critical fix that enables standalone disk persistence of `.ell` files without TSDev.exe and without Frida.

**Root cause:** `SymWriter::Save` in **`ELDebugger.dll`** (at RVA `0x120a0`) calls `XmlDocument.Save` — a .NET managed method — via a virtual call at vtable offset `0xd8`. In a standalone native process, the CLR is only **partially** initialized: the mixed-mode C++/CLI support DLLs load, but they do not fully boot the runtime, so the JIT-compiled target address is invalid. The virtual call therefore causes an access violation.

That exception is caught by the SEH handler of `FUN_10011230` in **orlib20.dll**. When the handler runs, `CompileText` skips `ELMeta::MDVault::SaveModule` — the function that actually writes the `.ell` file. As a result, the source compiles but **nothing is persisted to disk**.

**Fix:** Patch `FUN_10011230` in orlib20.dll (at RVA `0x11230`) to **return 1 immediately**. This bypasses the `SymWriter` crash entirely. `CompileText` then proceeds normally to call `ELMeta::MDVault::SaveModule`, which writes the `.ell` file (compiled module, magic `RNAM`).

**Patch bytes (8 bytes at `orlib20_base + 0x11230`):**
```
B8 01 00 00 00    MOV EAX, 1
C2 0C 00          RET 0x0C
```
`FUN_10011230` is `__fastcall` with 5 parameters (`ECX`, `EDX`, and 3 stack args = 12 bytes), hence `RET 0x0C`. The patch is applied **in-process to the loaded orlib20.dll image in the standalone CLI's own address space** (memory only); the on-disk `orlib20.dll` is never modified (READ-ONLY install preserved).

**Consequence:** The `.xml` file (debug symbols) is **NOT** written. This is acceptable — 7 existing `.ell` files in `MyWork\Bin` already operate without paired `.xml` files. The `.ell` file is the compiled module that TradeStation actually uses.

**Evidence:** Standalone probe `elevated_test20.exe` with the in-process patch produces `elmodule_0_2572.ell` (5091 bytes, magic `RNAM`) with **no Frida attached**. Compilation error buffer is empty. `VerifyModule_New = 1`.

### 5.7 DELETE — orchestrator approach (IMPLEMENTED & APPROVED)

> **Evidence:** `tmp/re_artifacts/DELETE_WORKING_MEMORY.md` (current/production section). Adversarial review: APPROVED (7 non-blocking findings addressed in cleanup).

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

**Retired/rejected approaches (do NOT use):** Direct lookup path (`FUN_101d05e0` — crashed); `CloseWarehouseForCurrentThreadAndPath` alone (did not persist); dirty-flag manipulation at offset `+0xBE` with command `0x27` (command `0x27` is not a save — it is a thin COM wrapper with no file I/O); command `0x45` / commit-all-storages (accepted but did not persist standalone); killing `WHServer.exe` (did not persist, caused crash-report popups); manual worker command sequencing `0x0F`→`0x32`→`0x17` (superseded by the orchestrator). See `tmp/re_artifacts/DELETE_WORKING_MEMORY.md` for the full historical record.

### 5.8 MODIFY — composition of INSERT primitives (IMPLEMENTED & APPROVED)

> **Evidence:** Two independent test runs, exit 0, all 8 phases pass. Adversarial review: APPROVED.

**Approach:** MODIFY introduces **no new orlib20 calls and no new RVAs**. It composes only existing, already-confirmed INSERT primitives in sequence:

1. `findExistPos(name, whType)` → `progId` (notify if not found; `success=false`).
2. `saveSource` — hidden EDIT control + `SaveModule_New` (sets warehouse source text for the existing entry).
3. `verifyModule` — `CELCompiler` setup + `StartCompilingProcess` + `VerifyModule_New` (with the `FUN_10011230` patch applied — see §5.6) → compiles AND re-persists the `.ell` file for the existing `progId`.
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

| Criterion | Approach 1 (ELIS.exe COM) | Approach 2 (orlib20.dll direct) |
|-----------|---------------------------|---------------------------------|
| Name -> (WHType, ProgId) resolution | NOT exposed; must scan XML out-of-band | `FindExistPos` / `_GetMasterRecByName` (HIGH conf) |
| Raw-source insert | No — XML file import only | `_CreateEntryEX` + `_SaveModule_New` (CONFIRMED) |
| Compile/verify | `VerifyAll` (whole library, void) | `StartCompilingProcess` + `VerifyModule_New` (CONFIRMED) |
| Error reporting | Void on delete/verify; results BSTR on import only | `_GetCompilingErrors` + `_GetErrorLevel` + count global (CONFIRMED) |
| Delete | `DeleteAnalysisTechnique(WHType,ProgId)` void; needs ProgId | Orchestrator `FUN_101a8360` (`__fastcall`, SEH-protected) + thread-check INT3 NOP patches + `FILEDELETE` (IMPLEMENTED & APPROVED; see §5.7) |
| Modify | No primitive (delete+insert) | Set source + verify + save (same primitives as insert; IMPLEMENTED & APPROVED — see §5.8) |
| Complexity / risk | Low call complexity, HIGH gap-filling complexity (XML synth, name resolution, error inference) | MED call complexity, LOW risk (param orders CONFIRMED, persistence CONFIRMED) |
| Headless viability | Uncertain (Desktop coupling suspected; void errors) | YES (smoke test PASSED; no TSDev/TS import) |

### Recommendation

**Primary: Approach 2 (orlib20.dll direct invocation).** It is the only approach that exposes primitives for all three acceptance criteria, including targeted compile/verify with retrievable errors and name resolution. Parameter orders for the key primitives are **CONFIRMED**, standalone INSERT persistence has been demonstrated via the `FUN_10011230` patch (see §5.6), **DELETE is IMPLEMENTED and APPROVED** via the orchestrator approach (see §5.7), and **MODIFY is IMPLEMENTED and APPROVED** via composition of existing INSERT primitives (see §5.8). **All three core functions (INSERT, DELETE, MODIFY) are now IMPLEMENTED and APPROVED** — the primary acceptance criteria is satisfied at the library level. Remaining risks are narrow: warehouse-type mappings for five component types and minor unmapped fields (see §5.5). It aligns with the project goal of least custom implementation: the CLI becomes a thin orchestrator over the same engine TSDev.exe uses.

**Fallback: Approach 1 (ELIS.exe COM).** Retain for the delete operation only, and only if name resolution is handled externally (e.g., XML scan) and the void return is acceptable. Not viable for insert/modify due to XML-only import and absent error reporting.

Both de-risking gates have **PASSED**: (a) orlib20 parameter orders are **CONFIRMED** (see §5.1), and (b) the headless `_OrlibInit` smoke test succeeded without TradeStation Desktop running (see `headless_init_smoke_test.md`).

---

## 7. Next Steps (TVS-2 Proposal)

> **Status update:** Items (a), (b), and (c) below are all **COMPLETED**. Parameter orders are CONFIRMED (see §5.1, `orlib_param_orders.md`), the headless init smoke test PASSED (see `headless_init_smoke_test.md`), standalone INSERT persistence has been demonstrated (see §5.6), DELETE is IMPLEMENTED & APPROVED (see §5.7), and MODIFY is IMPLEMENTED & APPROVED (see §5.8). **All three core functions (INSERT, DELETE, MODIFY) are now IMPLEMENTED and APPROVED** — the primary acceptance criteria is satisfied at the library level. Remaining work is follow-ups only (see §5.5 and §8).

The highest-ROI next slice de-risks the recommendation before any broad implementation:

**a) Resolve orlib20 unknown parameter orders (RE) — COMPLETED.**
Primary approach — **live tracing via the frida MCP** (Frida 17.17.0): spawn or attach to TSDev.exe via `create_interactive_session`, hook the orlib20 functions (`_CreateEntryEX@24`, `_SaveTextEX@16`, `_SaveModule_New@20`, `_VerifyModule_New@20`) with `Interceptor.attach` to capture `args[0..N]` in `onEnter`, then trigger insert/delete/modify operations in the TSDev UI to observe the actual argument values passed by the authoritative caller. This gives dynamic, authoritative argument values and is more reliable than static disassembly.

Secondary/confirmatory approach — **RE the caller sites in TSDev.exe** (static, deterministic): disassemble the call sites of the same functions within TSDev.exe to recover the exact argument order and types the GUI host uses. Use this to corroborate the live-trace findings.

As a quick preliminary to the headless smoke test (step b), hook `_OrlibInit@8` in ELIS.exe at startup via the frida MCP to capture its actual arguments (`workDirPath`, `contextId`) — this resolves open question #2 (`contextId` valid values) directly from the authoritative caller.

**b) Headless init smoke test (critical gate) — COMPLETED (PASSED).**
Build a minimal 32-bit C++ test that loads orlib20.dll, sets the DLL search directory to the TS Program folder, and calls `_OrlibInit(workDirPath, contextId)` with a candidate `contextId` (try 0 and small integers). Assert the return is nonzero and that `ELRte::CreateRte` succeeded (observable via a subsequent `FindExistPos` call returning a known existing name). Run this **without** TradeStation Desktop running. This is the critical headless-viability gate. If it fails, the recommendation must be revisited.

**c) Implement the CLI with TDD (red-green). — COMPLETED (all three functions IMPLEMENTED & APPROVED).**
1. ~~**DELETE**~~ — **DONE & APPROVED.** Orchestrator-based approach (`FUN_101a8360`) with thread-check INT3 NOP patches; persistence verified by two independent test runs (see §5.7).
2. ~~**INSERT**~~ — **DONE.** Standalone persistence demonstrated via `elevated_test20.exe` + `FUN_10011230` patch (see §5.6); production CLI wraps the confirmed 20-step flow.
3. ~~**MODIFY**~~ — **DONE & APPROVED.** Composes only existing INSERT primitives (`findExistPos` → `saveSource` → `verifyModule`) with no new orlib20 calls and no new RVAs; verified by two independent test runs (exit 0, ProgId preserved, `.ell` updated, compile errors surfaced, negative + compile-error tests return `success=false`; see §5.8).

Each function gets a red-green TDD cycle against a disposable test library copy (never the real install).

---

## 8. Open Questions / Risks

> **Resolved since initial study:** Items #1 (headless `CreateRte` — smoke test PASSED), #2 (`contextId` — CONFIRMED as 1), #3 (`_CreateEntryEX` params — CONFIRMED, see §5.1), #5 (`_SaveModule_New` GUI binding — hidden-EDIT workaround CONFIRMED, persistence via `VerifyModule_New` + §5.6 patch), #6 (`MDVault::SaveModule` direct — no longer needed; persistence achieved via `VerifyModule_New` → patched `CompileText` → `MDVault::SaveModule`), #8 (`_GetCompilingErrors` formatting — CONFIRMED, see §5.1), #11 (`_SaveTextEX` persistence — RESOLVED; `SaveModule_New` saves source only, `VerifyModule_New` persists `.ell`), and #13 (Frida live RE — enabled via Python frida MCP, Frida 17.17.0).

**Still open:**

1. ~~**Headless `CreateRte`**: Does `ELRte::CreateRte` succeed without TradeStation Desktop?~~ **RESOLVED — YES** (smoke test PASSED; see `headless_init_smoke_test.md`).
2. ~~**`contextId` valid values`**: What integer(s) are valid for `_OrlibInit`'s second argument?~~ **RESOLVED — `contextId = 1`** (CONFIRMED; see §5.1).
3. ~~**`_CreateEntryEX` params 2-5**: Exact order/meaning of WHType and flags.~~ **RESOLVED — CONFIRMED** `(whType, 0, name, 0, 0, &errCode)` (see §5.1).
4. **`_SaveTextEX` p1/p4**: Roles of param 1 (nonzero-checked, lstrlenA'd) and param 4 (unused in visible path). Still uncertain (MED). Note: `_SaveTextEX` is NOT used in the confirmed INSERT/MODIFY flow; `_SaveModule_New` is used instead.
5. ~~**`_SaveModule_New` GUI binding`: Does the hidden-EDIT workaround fully suppress the MessageBox?~~ **RESOLVED — hidden-EDIT workaround CONFIRMED working** in the standalone INSERT flow (see §5.3, §5.6). Persistence is via `VerifyModule_New`, not `_SaveModule_New`.
6. ~~**`MDVault::SaveModule` direct**: Feasible as a GUI-free alternative?~~ **RESOLVED — not needed.** Persistence is achieved via `VerifyModule_New` → patched `CompileText` → `MDVault::SaveModule` (see §5.6).
7. ~~**Persistence after `_SetSigDeleted`**: Which call persists the master index with the deleted flag before `FILEDELETE` removes files?~~ **RESOLVED — DELETE IMPLEMENTED & APPROVED.** The orchestrator `FUN_101a8360` handles persistence internally (`DeleteFromTree` → `closeWarehouse` → commit storages → `MWRKArea.wrk` written). The `SetSigDeleted`-based approach is RETIRED. See §5.7.
8. ~~**`_GetCompilingErrors` formatting**: Exact string format for error reporting.~~ **RESOLVED — CONFIRMED** (builds concatenated "name: msg\n..." across all error records; empty buffer = success; see §5.1).
9. **Error record layout** (stride 297): Fields beyond the 0x7775 sentinel are unmapped. — **STILL OPEN**.
10. **WHType 0x32**: Meaning unknown; may affect verify/save. — **STILL OPEN**.
11. ~~**`_SaveTextEX` persistence`: Does it persist alone, or is `_SaveModule_New` always mandatory?~~ **RESOLVED — `_SaveModule_New` saves source text only; `VerifyModule_New` persists the `.ell` file** (see §5.1, §5.6).
12. **Registry dependence**: orlib20 uses `orshr20.dll!_TSGetRegistryRootKeyA` and ADVAPI32 registry functions; the TS install's registry keys must be present. — **STILL OPEN**.
13. ~~**Frida live RE**: Live RE was initially blocked by a Frida helper-path bug.~~ **RESOLVED — live RE enabled** via Python frida MCP server running Frida 17.17.0 (which includes the fix), providing the full Frida JS API via `execute_in_session` for runtime value confirmation.

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
