// diag_m4_deps2.cpp
//
// DIAGNOSTIC ONLY (not part of the regression suite). Read-only with respect
// to production code: this file does NOT modify any src/ file. It writes a
// historical diagnostic for the rejected GetDependencies/Option B2 path;
// current production M.4 uses the linker and linked per-element dep-lists.
// It investigates THREE historical hypotheses for why
// GetDependencies (orlib20 ordinal 487) returns an empty dep list for a
// Strategy that calls a no-arg Function via `Value1 = FUNC_NAME;`:
//
//   (A) Missing link/flush step — TSDev may perform a separate step that
//       materializes dep records. Tested here by resolving and calling the
//       alternative orlib20 export `_GetStaticDependencyList@12` (ordinal
//       531) on both the Function's ProgId (callee key) and the Strategy's
//       ProgId (caller key). `_GetStaticDependencyList@12` walks an internal
//       dependency map that may be populated independently of the
//       `record->elements[i] + 0x7b` list that GetDependencies reads.
//
//   (B) Historical hypothesis: no-arg function refs were not encoded as 0xFA4
//       — a bare Function name without parentheses may be encoded differently.
//       Tested here by
//       inserting a Function WITH a numeric input and a Strategy that calls
//       it WITH parentheses: `Value1 = FUNC2(Close);`. If GetDependencies
//       returns the Function's ProgId in this case but not in the no-arg
//       case (diag_m4_deps.cpp), hypothesis B is supported.
//
//   (C) Dep list built lazily after save-to-disk + reload — the dep records
//       may only be materialized after a warehouse reload. Tested here by
//       inserting + verifying both items, then destroying the TsDevLib
//       instance and constructing a NEW TsDevLib instance (which re-runs
//       OrlibInit and re-loads the warehouse master records), then calling
//       GetDependencies on the Strategy.
//
// This diagnostic requires TradeStation Desktop to be open. If a crash report
// is detected, it is reported without terminating any process. Its 60-second
// limit is an operational timeout, not a measured completion time or M.4
// acceptance evidence.
//
// RE ABI references:
//   docs/RE/RE_M4_GETDEPENDENCIES_ABI.md  (GetDependencies, ordinal 487)
//   docs/RE/RE_M4_DEPLIST_ABI.md          (_GetStaticDependencyList@12 ord 531,
//                                          _DeleteDependencyList@4 ord 397)

#include "../src/orlib_resolver.h"          // OrlibResolver (public)
#include "../src/tsdev_lib_test_hooks.h"    // internal/test-only header
#include "tsdev_lib.h"
#include "process_inspection.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

using namespace tsdev;
using diag_m4_process::ProcessInspectionResult;

namespace {

ProcessInspectionResult reportCrashReporter(const char* phase) {
    std::printf("[%s] Checking for TSCrashReport.exe...\n", phase);
    const ProcessInspectionResult result =
        diag_m4_process::inspectProcessByName(L"TSCrashReport.exe");
    if (result == ProcessInspectionResult::Found) {
        std::printf("[%s] TSCrashReport.exe IS running (crash report detected)\n",
                    phase);
    } else if (result == ProcessInspectionResult::NotFound) {
        std::printf("[%s] TSCrashReport.exe NOT running (no crash report detected)\n",
                    phase);
    } else {
        std::fprintf(stderr,
                     "[%s] FATAL: process enumeration inspection failure for "
                     "TSCrashReport.exe\n",
                     phase);
    }
    return result;
}

// -------------------------------------------------------------------------
// SEH-protected call wrappers (POD-only so __try/__except is legal under
// MSVC C2712). Mirrors the wrapper pattern in diag_m4_deps.cpp.
// -------------------------------------------------------------------------

typedef HGLOBAL (__stdcall *GetDependenciesFn)(int whType, int progId,
                                               int elementIndex,
                                               int* outDepCount);
typedef int*    (__stdcall *GetStaticDepListFn)(int param_1, int param_2,
                                                int param_3);
typedef void    (__stdcall *DeleteDepListFn)(int* buf);

struct GetDependenciesSehResult { HGLOBAL handle; int depCount; };
bool callGetDependencies(GetDependenciesFn fn, int whType, int progId,
                         int elementIndex, GetDependenciesSehResult* out) {
    out->handle = NULL;
    out->depCount = 0;
    __try {
        out->handle = fn(whType, progId, elementIndex, &out->depCount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

struct GetStaticDepSehResult { int* buf; };
bool callGetStaticDepList(GetStaticDepListFn fn, int p1, int p2, int p3,
                          GetStaticDepSehResult* out) {
    out->buf = nullptr;
    __try {
        out->buf = fn(p1, p2, p3);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Real ComApi — binds the actual CoInitializeEx / CoUninitialize so the
// test-only ctor enters a real STA apartment (required for orlib20 / OLE).
ComApi realComApi() {
    return ComApi{&CoInitializeEx, &CoUninitialize};
}

// -------------------------------------------------------------------------
// EasyLanguage source builders.
//
// Historical hypothesis B requires a Function WITH a numeric input and a
// Strategy that calls it WITH parentheses. The standard EasyLanguage syntax for a function
// input declaration is `input: Price(Numeric);` (singular `input:` keyword,
// type-name `Numeric`). The function body assigns to the function name.
// The Strategy calls it as `Value1 = FUNC_NAME(Close);` (parenthesized call
// with the Close price series as the argument).
// -------------------------------------------------------------------------

// Function WITH a numeric input (hypothesis B callee).
std::string makeFunctionSourceWithInput(const std::string& name) {
    return std::string("{ ") + name + " }\r\n"
           "input: Price(Numeric);\r\n"
           + name + " = Price * 2;\r\n";
}

// Strategy that calls the named Function WITH parentheses (historical
// hypothesis B caller). Establishes a forward dep Strategy -> Function via a
// parenthesized call site, which was expected by that old probe to emit a 0xFA4
// function-ref opcode. Current M.4 evidence identifies compiler-written
// function references as 0xFA3 and production uses the linker path.
std::string makeStrategySourceCallingWithParens(
        const std::string& /*name*/, const std::string& calleeName) {
    return std::string("{ calls ") + calleeName + " }\r\n"
           "Value1 = " + calleeName + "(Close);\r\n"
           "Buy (\"EnterLong\") next bar at Market;\r\n";
}

void fail(const char* msg) {
    std::fprintf(stderr, "[FAIL] %s\n", msg);
}

// Pre-cleanup: delete the named item if it already exists (re-run safety).
void preCleanup(TsDevLib& lib, ComponentType type, const std::string& name) {
    int existingProgId = lib.findExistPos(name, type);
    if (existingProgId > 0) {
        std::printf("  Pre-cleanup: deleting existing %s (ProgId=%d)\n",
                    name.c_str(), existingProgId);
        lib.deleteEntry(type, name, /*flush=*/false);
    }
}

// Insert + verify a disposable item. Returns ProgId on success, 0 on failure.
int insertAndVerify(TsDevLib& lib, ComponentType type,
                    const std::string& name, const std::string& source,
                    const char* label) {
    preCleanup(lib, type, name);

    int errCode = 0;
    int progId = lib.createEntry(type, name, errCode);
    std::printf("      createEntry(%s, \"%s\") = %d, errCode = 0x%X\n",
                label, name.c_str(), progId, errCode);
    if (progId <= 0) {
        fail("createEntry returned ProgId <= 0");
        return 0;
    }

    if (!lib.saveSource(type, progId, source)) {
        std::fprintf(stderr, "      saveSource failed for %s\n",
                     name.c_str());
        lib.deleteEntry(type, name, /*flush=*/false);
        fail("saveSource returned false");
        return 0;
    }

    std::string errors = lib.verifyModule(type, progId);
    if (!errors.empty()) {
        std::fprintf(stderr,
                     "      verifyModule errors for %s:\n%s\n",
                     name.c_str(), errors.c_str());
        lib.deleteEntry(type, name, /*flush=*/false);
        fail("verifyModule returned compilation errors");
        return 0;
    }
    std::printf("      verifyModule(%s, %d): OK\n", label, progId);
    return progId;
}

// -------------------------------------------------------------------------
// GetDependencies probe (hypothesis B / C). Mirrors diag_m4_deps.cpp.
// Returns true if any call matched targetProgId.
// -------------------------------------------------------------------------
bool probeGetDependencies(GetDependenciesFn fn, const char* callerLabel,
                          int whType, int progId, int targetProgId) {
    std::printf("\n  --- GetDependencies(%s) whType=0x%X progId=%d "
                "targetProgId=%d ---\n",
                callerLabel, whType, progId, targetProgId);
    bool anyMatch = false;
    for (int elemIdx = 0; elemIdx <= 3; elemIdx++) {
        GetDependenciesSehResult depSeh;
        bool ok = callGetDependencies(fn, whType, progId, elemIdx, &depSeh);
        if (!ok) {
            std::printf("    elemIdx=%d: SEH raised\n", elemIdx);
            continue;
        }
        std::printf("    elemIdx=%d: handle=%p depCount=%d",
                    elemIdx, depSeh.handle, depSeh.depCount);
        if (depSeh.handle == NULL || depSeh.depCount <= 0) {
            std::printf(" (no deps / out-of-range)\n");
            if (depSeh.handle) GlobalFree(depSeh.handle);
            continue;
        }
        int* deps = (int*)GlobalLock(depSeh.handle);
        if (!deps) {
            std::printf(" (GlobalLock FAILED)\n");
            GlobalFree(depSeh.handle);
            continue;
        }
        bool matched = false;
        std::printf(" deps=[");
        for (int d = 0; d < depSeh.depCount; d++) {
            if (d > 0) std::printf(", ");
            std::printf("%d", deps[d]);
            if (deps[d] == targetProgId) {
                matched = true;
                anyMatch = true;
            }
        }
        std::printf("]");
        if (matched) {
            std::printf("  <<< MATCH (targetProgId=%d found)", targetProgId);
        }
        std::printf("\n");
        GlobalUnlock(depSeh.handle);
        GlobalFree(depSeh.handle);
    }
    if (anyMatch) {
        std::printf("  >> RESULT: targetProgId=%d WAS found in dep list of "
                    "%s (whType=0x%X progId=%d)\n",
                    targetProgId, callerLabel, whType, progId);
    } else {
        std::printf("  >> RESULT: targetProgId=%d NOT found in dep list of "
                    "%s (whType=0x%X progId=%d)\n",
                    targetProgId, callerLabel, whType, progId);
    }
    return anyMatch;
}

// -------------------------------------------------------------------------
// _GetStaticDependencyList@12 probe (hypothesis A — alternative API).
//
// ABI (docs/RE/RE_M4_DEPLIST_ABI.md):
//   int* __stdcall _GetStaticDependencyList@12(int param_1, int param_2,
//                                              int param_3);
//   - param_1: 0 = global tree walk; nonzero = single object pointer
//   - param_2: stored verbatim into header[0] of the returned buffer
//   - param_3: stored verbatim into header[1]; also the lookup key
//   - Returns: int* buffer (operator new), NULL on failure
//   - Format: {int header0; int header1;
//              struct{int a; int b;} entries[N];
//              int term0=-1; int term1=-1;}
//   - Free with: void __stdcall _DeleteDependencyList@4(int* buf);
//
// We call with param_1=0 (global tree walk), param_2=0, param_3=keyProgId.
// We then walk entries until terminator {0xFFFFFFFF, 0xFFFFFFFF} and check
// whether any entry's a or b field matches searchProgId.
// -------------------------------------------------------------------------
bool probeStaticDepList(GetStaticDepListFn getFn, DeleteDepListFn delFn,
                        const char* label, int keyProgId, int searchProgId) {
    std::printf("\n  --- _GetStaticDependencyList@12(%s) "
                "param_1=0 param_2=0 param_3(key)=%d searchProgId=%d ---\n",
                label, keyProgId, searchProgId);

    GetStaticDepSehResult r;
    bool ok = callGetStaticDepList(getFn, /*param_1=*/0, /*param_2=*/0,
                                   /*param_3=*/keyProgId, &r);
    if (!ok) {
        std::printf("    SEH raised\n");
        return false;
    }
    if (r.buf == nullptr) {
        std::printf("    returned NULL\n");
        return false;
    }
    int* p = r.buf;
    int header0 = p[0];
    int header1 = p[1];
    std::printf("    header[0]=%d (0x%X)  header[1]=%d (0x%X)\n",
                header0, (unsigned)header0, header1, (unsigned)header1);

    bool anyMatch = false;
    int idx = 2;  // entries start at int index 2 (byte offset 8)
    int entryCount = 0;
    while (true) {
        int a = p[idx];
        int b = p[idx + 1];
        if ((unsigned)a == 0xFFFFFFFFu && (unsigned)b == 0xFFFFFFFFu) {
            break;  // terminator
        }
        bool matched = (a == searchProgId) || (b == searchProgId);
        std::printf("    entry[%d]: a=%d (0x%X)  b=%d (0x%X)%s\n",
                    entryCount, a, (unsigned)a, b, (unsigned)b,
                    matched ? "  <<< MATCH" : "");
        if (matched) anyMatch = true;
        idx += 2;
        entryCount++;
        // Safety cap to avoid runaway read if the terminator is missing.
        if (entryCount > 4096) {
            std::printf("    [safety cap hit at 4096 entries, stopping]\n");
            break;
        }
    }
    std::printf("    total entries: %d\n", entryCount);
    if (anyMatch) {
        std::printf("  >> RESULT: searchProgId=%d WAS found in static dep "
                    "list (key=%d, %s)\n",
                    searchProgId, keyProgId, label);
    } else {
        std::printf("  >> RESULT: searchProgId=%d NOT found in static dep "
                    "list (key=%d, %s)\n",
                    searchProgId, keyProgId, label);
    }

    // Free the buffer via _DeleteDependencyList@4.
    __try {
        delFn(r.buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::printf("    [SEH raised while freeing buffer]\n");
    }
    return anyMatch;
}

// -------------------------------------------------------------------------
// Diagnostic state.
// -------------------------------------------------------------------------
struct DiagState {
    int exitCode = 0;
    // Hypothesis results.
    bool hypB_match = false;  // GetDependencies finds Function in Strategy
                              // dep list (parenthesized call)
    bool hypC_match = false;  // GetDependencies finds Function after reload
    bool hypA_funcKey_match = false;  // _GetStaticDependencyList@12 with
                                      // functionProgId as key finds strategy
    bool hypA_stratKey_match = false; // _GetStaticDependencyList@12 with
                                      // strategyProgId as key finds function
};

// -------------------------------------------------------------------------
// Worker thread body (64 MB stack — required by the EL compiler / hidden
// EDIT control, and TsDevLib is thread-affine).
// -------------------------------------------------------------------------
DWORD WINAPI runDiag(LPVOID param) {
    DiagState* st = static_cast<DiagState*>(param);

    const std::string funcName   = "TS_AUTO_M4_DIAG_FUNC2";
    const std::string stratName  = "TS_AUTO_M4_DIAG_STRAT2";

    std::printf("=== diag_m4_deps2: 3-hypothesis GetDependencies diagnostic "
                "===\n");
    std::printf("Hypothesis B: parenthesized call (Function WITH input)\n");
    std::printf("Hypothesis C: dep list materialized after warehouse reload\n");
    std::printf("Hypothesis A: _GetStaticDependencyList@12 alternative API\n");

    // --- Step 1: construct first TsDevLib (full init). No hooks. ---
    std::printf("\n[1] Constructing TsDevLib #1 (test-only ctor, real "
                "ComApi, no hooks)...\n");
    std::unique_ptr<TsDevLib> lib;
    try {
        TsDevLibTestConfig cfg;
        cfg.comApi        = realComApi();
        cfg.throwAfterCom = false;
        lib = tsdev::makeTsDevLibForTest(/*tsPathOverride=*/nullptr, cfg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: TsDevLib #1 construction threw: %s\n",
                     e.what());
        st->exitCode = 100;
        return 0;
    }
    if (!lib) {
        std::fprintf(stderr, "FATAL: makeTsDevLibForTest #1 returned null\n");
        st->exitCode = 101;
        return 0;
    }
    std::printf("    TsDevLib #1 constructed OK.\n");

    // --- Step 2: resolve exports via a separate OrlibResolver. ---
    std::printf("\n[2] Resolving exports via OrlibResolver...\n");
    GetDependenciesFn   getDependencies = nullptr;
    GetStaticDepListFn  getStaticDep    = nullptr;
    DeleteDepListFn     deleteDep       = nullptr;
    try {
        OrlibResolver resolver(L"orlib20.dll");
        getDependencies = reinterpret_cast<GetDependenciesFn>(
            resolver.resolveExport("GetDependencies"));
        // _GetStaticDependencyList@12 is ordinal 531 (named export).
        getStaticDep = reinterpret_cast<GetStaticDepListFn>(
            resolver.resolveExport("_GetStaticDependencyList@12"));
        // _DeleteDependencyList@4 is ordinal 397 (named export, alias of
        // _DeleteChangedModulesList@4 ordinal 396).
        deleteDep = reinterpret_cast<DeleteDepListFn>(
            resolver.resolveExport("_DeleteDependencyList@4"));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: OrlibResolver/resolveExport threw: %s\n",
                     e.what());
        st->exitCode = 102;
        return 0;
    }
    if (!getDependencies || !getStaticDep || !deleteDep) {
        std::fprintf(stderr,
                     "FATAL: failed to resolve one or more exports "
                     "(GetDependencies=%p _GetStaticDependencyList@12=%p "
                     "_DeleteDependencyList@4=%p)\n",
                     getDependencies, getStaticDep, deleteDep);
        st->exitCode = 103;
        return 0;
    }
    std::printf("    Resolved GetDependencies=%p\n", getDependencies);
    std::printf("    Resolved _GetStaticDependencyList@12=%p\n",
                getStaticDep);
    std::printf("    Resolved _DeleteDependencyList@4=%p\n", deleteDep);

    // --- Step 3: insert + verify the Function (callee) WITH a numeric
    // input. (Hypothesis B setup.) ---
    std::printf("\n[3] Insert + verify Function (callee, WITH input) "
                "\"%s\"...\n", funcName.c_str());
    int funcProgId = insertAndVerify(*lib, ComponentType::Function,
                                     funcName,
                                     makeFunctionSourceWithInput(funcName),
                                     "Function");
    if (funcProgId <= 0) {
        st->exitCode = 200;
        return 0;
    }

    // --- Step 4: insert + verify the Strategy (caller) that calls the
    // Function WITH parentheses. (Hypothesis B setup.) ---
    std::printf("\n[4] Insert + verify Strategy (caller, parenthesized call) "
                "\"%s\" calling Function...\n", stratName.c_str());
    int stratProgId = insertAndVerify(*lib, ComponentType::Strategy,
                                      stratName,
                                      makeStrategySourceCallingWithParens(
                                          stratName, funcName),
                                      "Strategy");
    if (stratProgId <= 0) {
        lib->deleteEntry(ComponentType::Function, funcName, /*flush=*/false);
        st->exitCode = 201;
        return 0;
    }

    std::printf("\n[5] Recorded ProgIds: Function=%d Strategy=%d\n",
                funcProgId, stratProgId);

    // =====================================================================
    // HYPOTHESIS B TEST — GetDependencies on the Strategy (caller) with
    // the Function's ProgId as the search target. The Strategy was built
    // with a PARENTHESIZED call `Value1 = FUNC2(Close);`. If GetDependencies
    // returns the Function's ProgId here (but not in diag_m4_deps.cpp's
    // no-arg case), hypothesis B is supported.
    // =====================================================================
    std::printf("\n[6] HYPOTHESIS B: GetDependencies on STRATEGY (caller, "
                "parenthesized call)...\n");
    {
        struct { int whType; const char* label; } stratWhTypes[] = {
            { 0,     "Strategy category (cat=0, subType=0 -> whType=0)" },
            { 0x200, "Strategy power-type 0x200 (FUN_101d43a0 case 0)" },
            { 4,     "Function category (whType=4, wrong type)" },
        };
        for (const auto& w : stratWhTypes) {
            std::printf("\n  >>> STRATEGY whType=0x%X  (%s)\n",
                        w.whType, w.label);
            if (probeGetDependencies(getDependencies, "Strategy",
                                     w.whType, stratProgId, funcProgId)) {
                st->hypB_match = true;
            }
        }
    }
    std::printf("\n[6] HYPOTHESIS B SUMMARY: %s\n",
                st->hypB_match
                    ? "FOUND Function ProgId in Strategy dep list for "
                      "parenthesized call (hypothesis B SUPPORTED — parens "
                      "DO emit a 0xFA4 function-ref opcode)"
                    : "DID NOT find Function ProgId in Strategy dep list "
                      "even with parens (hypothesis B NOT supported by "
                      "this evidence)");

    // =====================================================================
    // HYPOTHESIS A TEST — _GetStaticDependencyList@12 alternative API.
    //
    // Two probes:
    //   (a) key = functionProgId (the callee). If the returned entries
    //       contain the Strategy's ProgId in field a or b, this API can
    //       find reverse dependents (callers) of a Function.
    //   (b) key = strategyProgId (the caller). If the returned entries
    //       contain the Function's ProgId, this API returns forward deps.
    // =====================================================================
    std::printf("\n[7] HYPOTHESIS A: _GetStaticDependencyList@12 alternative "
                "API...\n");

    // (a) callee key -> look for caller (Strategy) ProgId
    std::printf("\n  [7a] key=functionProgId=%d (callee), "
                "search=strategyProgId=%d (caller)\n",
                funcProgId, stratProgId);
    if (probeStaticDepList(getStaticDep, deleteDep,
                           "key=function(callee), search=strategy(caller)",
                           funcProgId, stratProgId)) {
        st->hypA_funcKey_match = true;
    }

    // (b) caller key -> look for callee (Function) ProgId
    std::printf("\n  [7b] key=strategyProgId=%d (caller), "
                "search=functionProgId=%d (callee)\n",
                stratProgId, funcProgId);
    if (probeStaticDepList(getStaticDep, deleteDep,
                           "key=strategy(caller), search=function(callee)",
                           stratProgId, funcProgId)) {
        st->hypA_stratKey_match = true;
    }

    std::printf("\n[7] HYPOTHESIS A SUMMARY:\n");
    std::printf("  key=function(callee) found strategy(caller)? %s\n",
                st->hypA_funcKey_match ? "YES" : "NO");
    std::printf("  key=strategy(caller) found function(callee)? %s\n",
                st->hypA_stratKey_match ? "YES" : "NO");

    // =====================================================================
    // HYPOTHESIS C TEST — dep list materialized after warehouse reload.
    //
    // Destroy the current TsDevLib (which calls OrlibExit and tears down
    // the in-memory warehouse state), then construct a NEW TsDevLib
    // instance (which re-runs OrlibInit and re-loads the warehouse master
    // records from disk). Then call GetDependencies on the Strategy again.
    // If deps appear now (but didn't before reload), hypothesis C is
    // supported.
    // =====================================================================
    std::printf("\n[8] HYPOTHESIS C: reload warehouse via NEW TsDevLib "
                "instance...\n");

    // Re-resolve GetDependencies on the new resolver (the old resolver's
    // module handle is freed when lib #1 destructs? No — OrlibResolver
    // loads orlib20.dll with its own LoadLibraryW and frees it in its own
    // dtor. The TsDevLib destructor does NOT free the resolver's module.
    // But to be safe and to mirror the production pattern, we re-resolve
    // after the reload using a fresh resolver.)

    // Destroy TsDevLib #1.
    std::printf("  Destroying TsDevLib #1...\n");
    lib.reset();
    std::printf("  TsDevLib #1 destroyed (OrlibExit called).\n");

    // Construct TsDevLib #2 (reloads warehouse from disk).
    std::printf("  Constructing TsDevLib #2 (reloads warehouse)...\n");
    std::unique_ptr<TsDevLib> lib2;
    try {
        TsDevLibTestConfig cfg;
        cfg.comApi        = realComApi();
        cfg.throwAfterCom = false;
        lib2 = tsdev::makeTsDevLibForTest(/*tsPathOverride=*/nullptr, cfg);
    } catch (const std::exception& e) {
        std::fprintf(stderr,
                     "FATAL: TsDevLib #2 construction threw: %s\n",
                     e.what());
        st->exitCode = 300;
        return 0;
    }
    if (!lib2) {
        std::fprintf(stderr, "FATAL: makeTsDevLibForTest #2 returned null\n");
        st->exitCode = 301;
        return 0;
    }
    std::printf("  TsDevLib #2 constructed OK (warehouse reloaded).\n");

    // Re-resolve GetDependencies via a fresh OrlibResolver (lib2's own
    // resolver is private; we use a separate one as in diag_m4_deps.cpp).
    GetDependenciesFn getDependencies2 = nullptr;
    try {
        OrlibResolver resolver2(L"orlib20.dll");
        getDependencies2 = reinterpret_cast<GetDependenciesFn>(
            resolver2.resolveExport("GetDependencies"));
    } catch (const std::exception& e) {
        std::fprintf(stderr,
                     "FATAL: OrlibResolver #2 threw: %s\n", e.what());
        st->exitCode = 302;
        return 0;
    }
    if (!getDependencies2) {
        std::fprintf(stderr,
                     "FATAL: failed to resolve GetDependencies #2\n");
        st->exitCode = 303;
        return 0;
    }

    // Confirm both items still exist after reload (ProgIds should be
    // stable — they are warehouse record ids persisted on disk).
    int funcProgId2 = lib2->findExistPos(funcName, ComponentType::Function);
    int stratProgId2 = lib2->findExistPos(stratName, ComponentType::Strategy);
    std::printf("  After reload: Function ProgId=%d (was %d), "
                "Strategy ProgId=%d (was %d)\n",
                funcProgId2, funcProgId, stratProgId2, stratProgId);
    if (funcProgId2 <= 0 || stratProgId2 <= 0) {
        std::fprintf(stderr,
                     "  [WARN] one or both items not found after reload; "
                     "skipping GetDependencies probe on reloaded ids.\n");
    } else {
        // Use the reloaded ProgIds (in case they differ — they should not,
        // but be defensive).
        int funcIdUse = funcProgId2;
        int stratIdUse = stratProgId2;

        std::printf("\n  Probing GetDependencies on STRATEGY after reload "
                    "(whType=0)...\n");
        if (probeGetDependencies(getDependencies2, "Strategy(reloaded)",
                                 /*whType=*/0, stratIdUse, funcIdUse)) {
            st->hypC_match = true;
        }
        // Also try 0x200 after reload for completeness.
        std::printf("\n  Probing GetDependencies on STRATEGY after reload "
                    "(whType=0x200)...\n");
        if (probeGetDependencies(getDependencies2, "Strategy(reloaded)",
                                 /*whType=*/0x200, stratIdUse, funcIdUse)) {
            st->hypC_match = true;
        }
    }

    std::printf("\n[8] HYPOTHESIS C SUMMARY: %s\n",
                st->hypC_match
                    ? "Deps appeared after warehouse reload (hypothesis C "
                      "SUPPORTED — dep list is built lazily on reload)"
                    : "Deps still absent after warehouse reload "
                      "(hypothesis C NOT supported)");

    // =====================================================================
    // Cleanup: delete both items.
    // =====================================================================
    std::printf("\n[9] Cleanup: delete Strategy + Function...\n");
    if (lib2) {
        lib2->deleteEntry(ComponentType::Strategy, stratName,
                          /*flush=*/false);
        lib2->deleteEntry(ComponentType::Function, funcName,
                          /*flush=*/true);
    }
    std::printf("    Cleanup done.\n");

    // =====================================================================
    // Final summary.
    // =====================================================================
    std::printf("\n=== FINAL SUMMARY ===\n");
    std::printf("Function ProgId (callee, WITH input) = %d\n", funcProgId);
    std::printf("Strategy ProgId (caller, parens)    = %d\n", stratProgId);
    std::printf("\nHypothesis B (parenthesized call emits 0xFA4): %s\n",
                st->hypB_match ? "SUPPORTED" : "NOT supported");
    std::printf("Hypothesis C (dep list built lazily on reload): %s\n",
                st->hypC_match ? "SUPPORTED" : "NOT supported");
    std::printf("Hypothesis A (_GetStaticDependencyList@12):\n");
    std::printf("  key=function(callee) finds strategy(caller)? %s\n",
                st->hypA_funcKey_match ? "YES" : "NO");
    std::printf("  key=strategy(caller) finds function(callee)? %s\n",
                st->hypA_stratKey_match ? "YES" : "NO");

    std::printf("\n=== DIAGNOSIS ===\n");
    if (st->hypB_match) {
        std::printf("  - GetDependencies DOES return the Function's ProgId "
                    "when the Strategy calls it WITH parentheses. The "
                    "no-arg form (`Value1 = FUNC;`) likely does NOT emit "
                    "the 0xFA4 opcode — use parenthesized calls or a "
                    "Function with inputs to establish a recordable dep.\n");
    } else {
        std::printf("  - GetDependencies does NOT return the Function's "
                    "ProgId even with a parenthesized call to a Function "
                    "WITH inputs. The dep list is empty regardless of call "
                    "form.\n");
    }
    if (st->hypC_match) {
        std::printf("  - GetDependencies returns deps ONLY after a warehouse "
                    "reload (new TsDevLib). The dep records are materialized "
                    "lazily on master-record load, not at verify time.\n");
    } else {
        std::printf("  - GetDependencies returns no deps even after a "
                    "warehouse reload. The dep records are not materialized "
                    "by reload alone.\n");
    }
    if (st->hypA_funcKey_match) {
        std::printf("  - _GetStaticDependencyList@12 with key=function "
                    "ProgId RETURNS the Strategy (caller) ProgId in an "
                    "entry field. This API is the correct reverse-dependent "
                    "lookup mechanism.\n");
    } else if (st->hypA_stratKey_match) {
        std::printf("  - _GetStaticDependencyList@12 with key=strategy "
                    "ProgId RETURNS the Function (callee) ProgId in an "
                    "entry field. This API gives forward deps; reverse "
                    "dependents must be found by inverting over all "
                    "records' forward lists.\n");
    } else {
        std::printf("  - _GetStaticDependencyList@12 returned no matching "
                    "entries for either key. Either the dep map is empty "
                    "for these items, or the param_1/param_2/param_3 "
                    "values used are wrong (param_1=0 global walk, "
                    "param_2=0, param_3=ProgId).\n");
    }

    std::printf("\n=== RECOMMENDATION FOR REVERSE DEPENDENTS ===\n");
    if (st->hypA_funcKey_match) {
        std::printf("  Use _GetStaticDependencyList@12(0, 0, calleeProgId) "
                    "and scan entries for caller ProgIds. This is the "
                    "direct reverse-dependent API.\n");
    } else if (st->hypA_stratKey_match) {
        std::printf("  _GetStaticDependencyList@12(0, 0, callerProgId) "
                    "returns forward deps. To find reverse dependents, "
                    "iterate all candidate records and call this API with "
                    "each candidate's ProgId as key, checking whether the "
                    "target appears in the returned entries.\n");
    } else if (st->hypB_match || st->hypC_match) {
        std::printf("  GetDependencies works (under the conditions above). "
                    "Use the GetDependencies-based forward-dep inversion "
                    "traversal (enumerateReverseDependents pattern), "
                    "ensuring the call form / reload condition that makes "
                    "deps visible.\n");
    } else {
        std::printf("  NEITHER GetDependencies NOR _GetStaticDependencyList@12 "
                    "returned deps for the test items. Further RE (Frida "
                    "dynamic tracing of TSDev.exe's actual call sequence) "
                    "is required to determine how TSDev materializes and "
                    "reads dependency records.\n");
    }

    // Exit code: 0 if any hypothesis produced a match, 1 otherwise.
    bool anySuccess = st->hypB_match || st->hypC_match ||
                      st->hypA_funcKey_match || st->hypA_stratKey_match;
    st->exitCode = anySuccess ? 0 : 1;
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Pre-check: inspect TSCrashReport.exe without terminating it.
    if (reportCrashReporter("PRE") != ProcessInspectionResult::NotFound) {
        std::fprintf(stderr,
                     "FATAL: TSCrashReport.exe was detected or could not be "
                     "inspected before the diagnostic; no process was terminated.\n");
        return 97;
    }

    // Run on a 64 MB-stack worker thread (EasyLanguage compiler / hidden
    // EDIT control need this, and TsDevLib is thread-affine).
    DiagState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runDiag, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    // 60-second hard timeout — report a hang without terminating the worker.
    DWORD wait = WaitForSingleObject(hThread, 60000);
    if (wait == WAIT_TIMEOUT) {
        std::fprintf(stderr,
                     "\nFATAL: diagnostic timed out after 60s; worker was "
                     "not forcibly terminated and no process cleanup was "
                     "attempted. Manual exact-PID recovery is required.\n");
        CloseHandle(hThread);
        return 98;
    }
    if (wait == WAIT_FAILED) {
        std::fprintf(stderr,
                     "\nFATAL: waiting for diagnostic worker failed: %lu; "
                     "no thread or process was forcibly terminated.\n",
                     GetLastError());
        CloseHandle(hThread);
        return 98;
    }
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);

    // Post-check: did TSCrashReport.exe spawn (indicating a crash)?
    if (reportCrashReporter("POST") != ProcessInspectionResult::NotFound) {
        std::fprintf(stderr,
                     "FATAL: TSCrashReport.exe was detected or could not be inspected after the "
                     "diagnostic; no process cleanup was attempted.\n");
        return 97;
    }

    return st.exitCode;
}
