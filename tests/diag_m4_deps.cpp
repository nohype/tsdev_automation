// diag_m4_deps.cpp
//
// DIAGNOSTIC ONLY (not part of the regression suite).
// Historical investigation of the rejected GetDependencies/Option B2 path;
// the current production implementation uses the linker and linked dep-lists.
//
// Purpose: investigate why the historical enumerateReverseDependents() path in src/tsdev_lib.cpp
// finds ZERO reverse dependents in the M.4 integration test, even though a
// Strategy is inserted that calls a Function.
//
// Historical enumerateReverseDependents inverted the FORWARD dependency list: for each
// candidate record it called GetDependencies(candidateWhType, candidateProgId,
// elementIndex, &depCount) and checked if the root ProgId appeared in the
// returned int array. The historical M.4 test reported the sentinel firing
// (traversal ran) but no per-dependent events — i.e. GetDependencies never
// returned the Function's ProgId for any candidate.
//
// This diagnostic:
//   1. Constructs a real TsDevLib (test-only ctor, real ComApi, no hooks) on
//      a 64 MB-stack worker thread (full init: patches, OrlibInit, EDIT ctrl).
//   2. Inserts + verifies a Function "TS_AUTO_M4_DIAG_FUNC" with body
//      `TS_AUTO_M4_DIAG_FUNC = 1;`.
//   3. Inserts + verifies a Strategy "TS_AUTO_M4_DIAG_STRAT" that calls the
//      Function: `Value1 = TS_AUTO_M4_DIAG_FUNC;` + a market buy order.
//   4. Records both ProgIds.
//   5. Constructs a separate OrlibResolver on the same orlib20.dll and
//      resolves GetDependencies (ordinal 487 / by name).
//   6. For the Strategy (caller), calls GetDependencies with several whType
//      values (0 = Strategy category, 0x200 = Strategy power-type from the
//      RE note's FUN_101d43a0 table, 4 = Function category) and several
//      elementIndex values (0..3). Prints return value, depCount, and ALL
//      dep IDs. Flags whether any dep ID matches the Function's ProgId.
//   7. For the Function (callee), calls GetDependencies similarly (expected
//      no deps).
//   8. Cleans up: deletes both items.
//   9. Reports TSCrashReport.exe status before and after; no process cleanup is
//      attempted automatically.
//
// This historical diagnostic does NOT modify production code. Its 60-second
// limit is an operational timeout; a timeout is reported without forcibly
// terminating the worker or any process. It is not a measured result or an
// M.4 acceptance result.
//
// RE ABI reference: docs/RE/RE_M4_GETDEPENDENCIES_ABI.md
//   HGLOBAL __stdcall GetDependencies(int whType, int progId,
//                                     int elementIndex, int *outDepCount);
//   - whType is the CATEGORY (0=Strategy,1=ActivityBar,2=Indicator,
//     3=PaintBar,4=Function). FUN_101d43a0 switch only handles 0..4;
//     expanded whTypes (e.g. 0x1402 for TradingApp) hit the default case
//     and return no deps.
//   - Historical diagnostic expectation: a GMEM_MOVEABLE HGLOBAL containing
//     *outDepCount int32s (function ProgIds from the element's compiled
//     bytecode), with opcode 0xFA4. The M.4 evidence later found the compiler
//     writes 0xFA3 instead; this old probe is retained only for provenance.
//     NULL + *outDepCount==0 means no deps / out-of-range elementIndex.
//     Caller must GlobalLock/GlobalUnlock/GlobalFree.

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

// --- SEH-protected call wrapper (POD-only so __try/__except is legal
// under MSVC C2712). Mirrors the wrapper in tsdev_lib.cpp / diag_m4_enum_speed.
typedef HGLOBAL (__stdcall *GetDependenciesFn)(int whType, int progId,
                                               int elementIndex,
                                               int* outDepCount);

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

// Real ComApi — binds the actual CoInitializeEx / CoUninitialize so the
// test-only ctor enters a real STA apartment (required for orlib20 / OLE).
ComApi realComApi() {
    return ComApi{&CoInitializeEx, &CoUninitialize};
}

// --- EasyLanguage source builders (mirror test_reverse_dependent_revalidation
// patterns, which are proven to compile). ---

// Function: body assigns a constant. The function name in the body MUST
// match the item name.
std::string makeFunctionSource(const std::string& name) {
    return name + " = 1;\r\n";
}

// Strategy that calls the named (no-input) Function in its order expression.
// Establishes a forward dep Strategy -> Function.
std::string makeStrategySourceCalling(const std::string& /*name*/,
                                      const std::string& calleeName) {
    return std::string("{ calls ") + calleeName + " }\r\n"
           "Value1 = " + calleeName + ";\r\n"
           "Buy (\"EnterLong\") next bar at Market;\r\n";
}

// Print a failure banner.
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

// Probe GetDependencies for one (whType, progId) across elementIndex 0..3.
// Prints return value, depCount, all dep IDs, and whether any dep ID matches
// targetProgId. Returns true if any call matched targetProgId.
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
            // NULL at elemIdx 0 means no deps at all; for elemIdx>0 NULL is
            // the expected end sentinel. Continue probing to be thorough.
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

struct DiagState {
    int exitCode = 0;
};

DWORD WINAPI runDiag(LPVOID param) {
    DiagState* st = static_cast<DiagState*>(param);

    const std::string funcName   = "TS_AUTO_M4_DIAG_FUNC";
    const std::string stratName  = "TS_AUTO_M4_DIAG_STRAT";

    std::printf("=== diag_m4_deps: GetDependencies reverse-dependent "
                "diagnostic ===\n");

    // --- Step 1: construct a real TsDevLib (full init). No hooks. ---
    std::printf("\n[1] Constructing TsDevLib (test-only ctor, real ComApi, "
                "no hooks)...\n");
    std::unique_ptr<TsDevLib> lib;
    try {
        TsDevLibTestConfig cfg;
        cfg.comApi        = realComApi();
        cfg.throwAfterCom = false;
        // No hooks — we drive GetDependencies directly via a separate
        // OrlibResolver, not via verifyModule's traversal.
        lib = tsdev::makeTsDevLibForTest(/*tsPathOverride=*/nullptr, cfg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: TsDevLib construction threw: %s\n",
                     e.what());
        st->exitCode = 100;
        return 0;
    }
    if (!lib) {
        std::fprintf(stderr, "FATAL: makeTsDevLibForTest returned null\n");
        st->exitCode = 101;
        return 0;
    }
    std::printf("    TsDevLib constructed OK.\n");

    // --- Step 2: resolve GetDependencies via a separate OrlibResolver. ---
    std::printf("\n[2] Resolving GetDependencies via OrlibResolver...\n");
    GetDependenciesFn getDependencies = nullptr;
    try {
        OrlibResolver resolver(L"orlib20.dll");
        getDependencies = reinterpret_cast<GetDependenciesFn>(
            resolver.resolveExport("GetDependencies"));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: OrlibResolver/resolveExport threw: %s\n",
                     e.what());
        st->exitCode = 102;
        return 0;
    }
    if (!getDependencies) {
        std::fprintf(stderr, "FATAL: failed to resolve GetDependencies\n");
        st->exitCode = 103;
        return 0;
    }
    std::printf("    Resolved GetDependencies=%p\n", getDependencies);

    // --- Step 3: insert + verify the Function (callee). ---
    std::printf("\n[3] Insert + verify Function (callee) \"%s\"...\n",
                funcName.c_str());
    int funcProgId = insertAndVerify(*lib, ComponentType::Function,
                                     funcName,
                                     makeFunctionSource(funcName),
                                     "Function");
    if (funcProgId <= 0) {
        st->exitCode = 200;
        return 0;
    }

    // --- Step 4: insert + verify the Strategy (caller) that calls the
    // Function. ---
    std::printf("\n[4] Insert + verify Strategy (caller) \"%s\" calling "
                "Function...\n", stratName.c_str());
    int stratProgId = insertAndVerify(*lib, ComponentType::Strategy,
                                      stratName,
                                      makeStrategySourceCalling(stratName,
                                                                funcName),
                                      "Strategy");
    if (stratProgId <= 0) {
        lib->deleteEntry(ComponentType::Function, funcName, /*flush=*/false);
        st->exitCode = 201;
        return 0;
    }

    std::printf("\n[5] Recorded ProgIds: Function=%d Strategy=%d\n",
                funcProgId, stratProgId);

    // --- Step 6: probe GetDependencies on the Strategy (caller) with
    // different whType values. The reverse-dependent traversal in
    // enumerateReverseDependents computes candidateWhType as
    // `cat | (subType << 8)`. For a plain Strategy (cat=0, subType=0) that
    // is 0. We also try 0x200 (the Strategy power-type from FUN_101d43a0
    // case 0) and 4 (Function category) to see what works. ---
    std::printf("\n[6] Probing GetDependencies on the STRATEGY (caller) "
                "with multiple whType values...\n");
    bool stratMatchedAny = false;
    struct { int whType; const char* label; } stratWhTypes[] = {
        { 0,     "Strategy category (cat=0, subType=0 -> expanded whType=0)" },
        { 0x200, "Strategy power-type 0x200 (FUN_101d43a0 case 0 power[1])" },
        { 4,     "Function category (whType=4, wrong type for a Strategy)" },
    };
    for (const auto& w : stratWhTypes) {
        std::printf("\n  >>> STRATEGY whType=0x%X  (%s)\n", w.whType, w.label);
        if (probeGetDependencies(getDependencies, "Strategy",
                                 w.whType, stratProgId, funcProgId)) {
            stratMatchedAny = true;
        }
    }
    std::printf("\n[6] SUMMARY for Strategy: %s\n",
                stratMatchedAny
                    ? "FOUND Function ProgId in dep list for at least one whType"
                    : "DID NOT find Function ProgId in dep list for ANY whType");

    // --- Step 7: probe GetDependencies on the Function (callee). Expected
    // no deps (a leaf Function that just returns 1 calls nothing). ---
    std::printf("\n[7] Probing GetDependencies on the FUNCTION (callee) "
                "with multiple whType values (expected: no deps)...\n");
    struct { int whType; const char* label; } funcWhTypes[] = {
        { 4, "Function category (whType=4, correct for a Function)" },
        { 0, "Strategy category (whType=0, wrong type for a Function)" },
    };
    for (const auto& w : funcWhTypes) {
        std::printf("\n  >>> FUNCTION whType=0x%X  (%s)\n", w.whType, w.label);
        // Function has no callers we care about here; targetProgId is the
        // Strategy's (which would NOT be a dep of the Function — the dep
        // direction is caller->callee, so the Strategy's deps include the
        // Function, not vice versa). Pass stratProgId just to confirm it
        // is NOT present.
        probeGetDependencies(getDependencies, "Function",
                             w.whType, funcProgId, stratProgId);
    }

    // --- Step 8: final summary. ---
    std::printf("\n=== FINAL SUMMARY ===\n");
    std::printf("Function ProgId (callee) = %d\n", funcProgId);
    std::printf("Strategy ProgId (caller) = %d\n", stratProgId);
    std::printf("Strategy dep list contains Function ProgId? %s\n",
                stratMatchedAny ? "YES" : "NO");
    std::printf("\n=== DIAGNOSIS ===\n");
    if (stratMatchedAny) {
        std::printf("  - GetDependencies DOES return the Function's ProgId "
                    "in the Strategy's dep list for at least one whType.\n");
        std::printf("  - The reverse-dependent traversal's failure to find "
                    "the Strategy is therefore NOT a GetDependencies data "
                    "problem; it is a traversal-side problem (wrong whType "
                    "computed, wrong elementIndex range, or refreshCache "
                    "not making the new records visible to "
                    "_LoadMasterRecsEX).\n");
    } else {
        std::printf("  - GetDependencies does NOT return the Function's "
                    "ProgId in the Strategy's dep list for ANY probed "
                    "whType.\n");
        std::printf("  - Possible causes:\n");
        std::printf("    * The Strategy's compiled bytecode does not record "
                    "a 0xFA4 function-ref opcode for a no-arg Function "
                    "reference (maybe EL only emits 0xFA4 for Functions "
                    "with inputs, and a no-arg Function ref is encoded "
                    "differently).\n");
        std::printf("    * The dependency list is built lazily and not yet "
                    "populated immediately after verify (needs a "
                    "refreshCache / cmd 0x4E flush first).\n");
        std::printf("    * The whType passed by the traversal is wrong "
                    "(expanded vs category mismatch) AND none of 0/0x200/4 "
                    "is the right value either.\n");
    }

    // --- Step 9: cleanup. ---
    std::printf("\n[9] Cleanup: delete Strategy + Function...\n");
    lib->deleteEntry(ComponentType::Strategy, stratName, /*flush=*/false);
    lib->deleteEntry(ComponentType::Function, funcName, /*flush=*/true);
    std::printf("    Cleanup done.\n");

    st->exitCode = stratMatchedAny ? 0 : 1;
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Pre-check: inspect TSCrashReport.exe without terminating it.
    if (reportCrashReporter("PRE") != ProcessInspectionResult::NotFound) {
        std::fprintf(stderr,
                     "FATAL: TSCrashReport.exe was detected or could not be "
                     "inspected before the diagnostic; no process was "
                     "terminated.\n");
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
                     "FATAL: TSCrashReport.exe was detected or could not be "
                     "inspected after the diagnostic; no process cleanup was "
                     "attempted.\n");
        return 97;
    }

    return st.exitCode;
}
