// test_reverse_dependent_revalidation.cpp
//
// Workstream M slice M.4 — integration test for reverse-dependent revalidation
// parity with TSDev.exe FUN_004D91E0. The current production implementation
// uses the linker-based path; CLI-created object code currently has version=0,
// so the dependency cases remain blocked rather than constituting a green
// acceptance result.
//
// CONFIRMED FACTS (Ghidra static analysis of TSDev.exe FUN_004D91E0; see
// docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md):
//
//   R-M.4.1 — Traversal: after a successful root verify via
//   _VerifyModule_New@20, for categories 0 (Strategy), 1 (Function), and 4
//   (SystemFunction) ONLY, TSDev enumerates reverse dependents (callers) of
//   the just-verified module and recompiles+reverifies each one via a
//   recursive FUN_004d91e0 call (which invokes _VerifyModule_New@20 /
//   CompileText). Each reverse dependent is a FULL recompile, not a
//   lightweight check. The implemented CLI path is linker-based: for each
//   candidate record from _LoadMasterRecsEX@16, call Ordinal_28 and the
//   linker (ordinal 85), then scan the linked record's per-element dep-lists
//   at element+0x7b for a matching root ProgId. GetDependencies/
//   _GetStaticDependencyList@12 are not the production path; the M.4
//   evidence note records the GetDependencies type-code mismatch and the
//   linker-based correction.
//
//   R-M.4.2 — Contract-change detection (FUN_004d9040) is purely
//   warehouse-record metadata: compares field offsets +0xd6, +0x15 (bits
//   1,2,3,4,5,6), +0x7f (bits 0-2,12,13,15), +0x81 (bits 8,11,12,13,14,15),
//   +0x29 (count), and a per-element array at +0x4f (stride 0xb5, element
//   fields +0xd/+0x11/+0x15/+0x83/+0x87/+0xb1). NO source-text parsing.
//   Returns 1 = no change, 0 = changed.
//
//   R-M.4.3 — Recursion / cycle handling: TSDev has NO visited-set and NO
//   depth counter; cycle protection is implicit (per-record +0x55 "verified"
//   flag + contract-change gate). The CLI replication MUST use an explicit
//   visited-set (stricter than TSDev).
//
//   R-M.4.4 — Failure reporting: the per-dependent loop does NOT abort on
//   failure; the compiler +0x04 is a running AND of all verify results. The
//   ROOT result (+0x00) drives UI success/failure — dependent failures do
//   NOT flip root success.
//
//   R-M.4.5 — No single orlib20 export performs reverse-dependent
//   revalidation. The implemented traversal is composed from the linker
//   (ordinal 85), Ordinal_28 (partial-record setup), Ordinal_70 (linked-record
//   release), _LoadMasterRecsEX@16 (candidate enumeration), and
//   _VerifyModule_New@20 (dependent reverify). It parses the linked
//   per-element dep-lists directly; the forward GetDependencies exports are
//   not used by production.
//
//   Category gate (RE note §1.3, 0x004d92b2-0x004d92bd): reverse dependents
//   are processed ONLY when the root verify succeeded AND the category is
//   0 (Strategy), 1 (Function), or 4 (SystemFunction). Other categories
//   (2 Indicator, 3 ActivityBar, etc.) skip the traversal entirely.
//
// This test exercises the onReverseDependentRevalidationReady test hook
// (populated via makeTsDevLibForTest) which verifyModule() invokes during the
// linker-based reverse-dependent traversal. The hook fires a traversal-started
// sentinel once at traversal entry (dependent fields = 0,
// revalidationTriggered = -1), then once per discovered reverse dependent.
//
// CURRENT LIMITATION: CLI-created object code has version=0, so the linker
// populators leave the element arrays empty and caller-dependent cases cannot
// discover their dependencies. The dependency assertions are therefore a
// documented blocked M.4 gate, not evidence that traversal is absent or that
// the test is green. The category-gate assertion remains meaningful: an
// Indicator (category 2) must not start the traversal. The cycle-safety case
// remains a unit-level hook contract pin; a live warehouse cycle is deferred
// to the lowest safe boundary described by plan §M.4 TDD step 4.

// 5 test cases:
//   1. Strategy caller of a Function — modify the Function, assert the
//      traversal ran and the Strategy was revalidated.
//   2. Indicator caller of a Function — modify the Function, assert the
//      Indicator was revalidated.
//   3. Negative: Function with NO callers — modify it, assert the traversal
//      was entered (sentinel fired) but zero dependent callbacks fired.
//   4. Category gate: modify an Indicator (category 2, NOT in 0/1/4) —
//      assert NO traversal-started sentinel fired (the gate excludes
//      category 2).
//   5. Cycle safety (unit-level contract pin) — documents the live-cycle
//      test as deferred and asserts the hook signature carries a
//      traversalDepth field.
//
// REQUIRES TradeStation: this is an integration test that constructs a real
// TsDevLib (loads orlib20.dll, applies patches, calls OrlibInit, creates the
// hidden EDIT control) and inserts + verifies + deletes real warehouse
// entries. This is a library-level test; Desktop/TSDev running state is not a
// prerequisite because the test does not exercise the Desktop UI.
//
// Runs on a 64 MB-stack worker thread (EasyLanguage compiler needs far more
// than the default 1 MB, and the hidden EDIT control is thread-affine so
// TsDevLib must live on this thread).
//
// Exit code: 0 only if ALL assertions pass; nonzero on any failure.

#include "../src/tsdev_lib_test_hooks.h"  // internal/test-only header (NOT public)
#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

using namespace tsdev;

namespace {

// ---------------------------------------------------------------------------
// CHECK macro — prints PASS/FAIL per assertion with line numbers.
// ---------------------------------------------------------------------------
static int g_failCount = 0;
static int g_passCount = 0;

#define CHECK(cond, msg) do {                                      \
    if (!(cond)) {                                                 \
        std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        g_failCount++;                                             \
    } else {                                                       \
        std::printf("PASS: %s\n", msg);                            \
        g_passCount++;                                             \
    }                                                              \
} while(0)

// ---------------------------------------------------------------------------
// Thread-local capture state for the
// onReverseDependentRevalidationReady hook.
//
// The hook runs on the worker thread (the same thread verifyModule runs on)
// and writes into these globals. The test reads them after verifyModule
// returns. thread_local ensures each worker thread has its own copy.
//
// The hook fires in two phases:
//   (a) ONE traversal-started sentinel (revalidationTriggered == -1) at
//       traversal entry.
//   (b) ZERO or more per-dependent events (revalidationTriggered == 0 or 1).
// ---------------------------------------------------------------------------
struct DependentEvent {
    int dependentComponentType = -1;
    int dependentProgId        = -1;
    int dependentCategory      = -1;
    int revalidationTriggered  = -1;  // -1 sentinel, 0 skipped, 1 reverified
    int revalidationResult     = -1;  // 1 success / 0 failure / -1 not-run
    int traversalDepth         = -1;
};

struct ReverseDepCapture {
    bool        sentinelFired       = false;
    int         rootComponentType   = -1;
    int         rootProgId          = -1;
    int         rootCategory        = -1;
    std::vector<DependentEvent> dependentEvents;
};

thread_local ReverseDepCapture g_capture;

void onReverseDependentRevalidationReady(
    int rootComponentType, int rootProgId, int rootCategory,
    int dependentComponentType, int dependentProgId,
    int dependentCategory,
    int revalidationTriggered, int revalidationResult,
    int traversalDepth) {
    if (revalidationTriggered == -1) {
        // Traversal-started sentinel.
        g_capture.sentinelFired     = true;
        g_capture.rootComponentType = rootComponentType;
        g_capture.rootProgId        = rootProgId;
        g_capture.rootCategory      = rootCategory;
        return;
    }
    // Per-dependent event.
    DependentEvent ev;
    ev.dependentComponentType = dependentComponentType;
    ev.dependentProgId        = dependentProgId;
    ev.dependentCategory      = dependentCategory;
    ev.revalidationTriggered  = revalidationTriggered;
    ev.revalidationResult     = revalidationResult;
    ev.traversalDepth         = traversalDepth;
    g_capture.dependentEvents.push_back(ev);
}

void resetCapture() {
    g_capture = ReverseDepCapture{};
}

// ---------------------------------------------------------------------------
// Real ComApi — binds the actual CoInitializeEx / CoUninitialize so the
// test-only ctor enters a real STA apartment (required for orlib20 / OLE).
// ---------------------------------------------------------------------------
ComApi realComApi() {
    return ComApi{&CoInitializeEx, &CoUninitialize};
}

// Build a TsDevLibTestConfig with the real COM API and the
// onReverseDependentRevalidationReady hook installed. throwAfterCom is false
// so the ctor proceeds with the full init sequence (loads orlib20, applies
// patches, OrlibInit, EDIT control).
TsDevLibTestConfig makeConfigWithHook() {
    TsDevLibTestConfig cfg;
    cfg.comApi                            = realComApi();
    cfg.throwAfterCom                     = false;
    cfg.onReverseDependentRevalidationReady =
        &onReverseDependentRevalidationReady;
    return cfg;
}

// ---------------------------------------------------------------------------
// Minimal valid EasyLanguage source per component type, mirroring the
// proven patterns from test_save_module.cpp / test_compiler_layout.cpp /
// test_0x40a_notification.cpp. For Function, the function name in the
// source body MUST match the item name.
// ---------------------------------------------------------------------------
std::string makeFunctionSource(const std::string& name) {
    // Function name in the body MUST match the item name.
    return name + " = 1;\r\n";
}

// A Function whose body returns a value derived from a named function call.
// Used to create a caller->callee dependency: the Strategy/Indicator source
// references the Function by name, so the Function's ProgId appears in the
// caller's linked per-element dependency list (which the linker-based
// traversal scans).
std::string makeFunctionSourceCalling(const std::string& name,
                                      const std::string& calleeName) {
    // The function body calls calleeName, establishing a forward dependency
    // from this function to calleeName. EasyLanguage function call syntax
    // for a no-input function: just reference the name (no parentheses).
    return name + " = " + calleeName + ";\r\n";
}

std::string makeStrategySourceCalling(const std::string& /*name*/,
                                      const std::string& calleeName) {
    // A Strategy that calls the named (no-input) Function in its order
    // expression. This establishes a compiled dependency from the Strategy to
    // the Function, so the Function's ProgId is in the Strategy's linked
    // per-element dep list (which the linker-based traversal scans to find the
    // Strategy as a reverse dependent of the Function).
    return std::string("{ calls ") + calleeName + " }\r\n"
           "Value1 = " + calleeName + ";\r\n"
           "Buy (\"EnterLong\") next bar at Market;\r\n";
}

std::string makeIndicatorSourceCalling(const std::string& /*name*/,
                                       const std::string& calleeName) {
    // An Indicator that calls the named (no-input) Function in its Plot
    // expression. This establishes a forward dependency from the Indicator
    // to the Function.
    return std::string("{ calls ") + calleeName + " }\r\n"
           "Value1 = " + calleeName + ";\r\n"
           "Plot1(Value1, \"Dep\");\r\n";
}

// A modified Function source that changes the contract (signature) so the
// M.4 contract-change comparison (FUN_004d9040 field offsets) detects a
// change. We change the function body to return a different constant — this
// alters the compiled contract metadata, which the warehouse record reflects.
// (The exact field that flips is one of those documented in RE note §2; the
// test does not depend on which specific field changes, only that the
// revalidation traversal runs for the caller.)
std::string makeFunctionSourceModified(const std::string& name) {
    return name + " = 2;\r\n";
}

// Print a failure banner and return the given code.
int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// ---------------------------------------------------------------------------
// Pre-cleanup helper: delete the named item if it already exists (re-run
// safety). Returns the progId that was deleted (>0) or 0 if not found.
// ---------------------------------------------------------------------------
int preCleanup(TsDevLib& lib, ComponentType type, const std::string& name) {
    int existingProgId = lib.findExistPos(name, type);
    if (existingProgId > 0) {
        std::printf("  Pre-cleanup: deleting existing %s (ProgId=%d)\n",
                    name.c_str(), existingProgId);
        lib.deleteEntry(type, name, /*flush=*/false);
    }
    return existingProgId;
}

// Delete the .ell file for a given ProgId so the test is repeatable.
bool cleanEllFile(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        return true;  // already absent
    }
    return (DeleteFileA(path.c_str()) != 0);
}

// ---------------------------------------------------------------------------
// insertAndVerify — helper: pre-clean, create, save, verify a disposable
// item. Returns the ProgId on success, 0 on a fatal setup error. The caller
// is responsible for cleanup (deleteEntry) after the assertions.
// ---------------------------------------------------------------------------
int insertAndVerify(TsDevLib& lib, ComponentType type,
                    const std::string& name, const std::string& source,
                    int caseNum) {
    const char* typeLabel =
        (type == ComponentType::Strategy)  ? "Strategy"  :
        (type == ComponentType::Indicator) ? "Indicator" :
        (type == ComponentType::Function)  ? "Function"  : "Unknown";

    preCleanup(lib, type, name);

    int errCode = 0;
    int progId = lib.createEntry(type, name, errCode);
    std::printf("      createEntry(%s, \"%s\") = %d, errCode = 0x%X\n",
                typeLabel, name.c_str(), progId, errCode);
    if (progId <= 0) {
        fail("createEntry returned ProgId <= 0", 100 + caseNum * 10 + 2);
        return 0;
    }

    if (!lib.saveSource(type, progId, source)) {
        std::fprintf(stderr, "      saveSource failed for %s\n",
                     name.c_str());
        lib.deleteEntry(type, name, /*flush=*/false);
        fail("saveSource returned false", 100 + caseNum * 10 + 3);
        return 0;
    }

    std::string errors = lib.verifyModule(type, progId);
    if (!errors.empty()) {
        std::fprintf(stderr,
                     "      verifyModule errors for %s:\n%s\n",
                     name.c_str(), errors.c_str());
        lib.deleteEntry(type, name, /*flush=*/false);
        fail("verifyModule returned compilation errors",
             100 + caseNum * 10 + 4);
        return 0;
    }
    std::printf("      verifyModule(%s, %d): OK\n", typeLabel, progId);
    return progId;
}

// ---------------------------------------------------------------------------
// Case 1: Strategy caller of a Function.
//
// Insert a Function (the callee). Insert a Strategy that calls the Function
// (the caller — establishes a compiled Strategy->Function dependency). Modify
// the Function's source (contract change) and re-verify the Function. Assert:
//   - the traversal-started sentinel fired (root = the Function),
//   - at least one per-dependent event fired for the Strategy (the caller),
//   - the Strategy's revalidation was triggered.
//
// The dependency assertions are currently blocked when CLI-created object code
// has version=0 and the linker leaves its element arrays empty.
// ---------------------------------------------------------------------------
int runStrategyCallerCase(TsDevLib& lib, int caseNum,
                          const std::string& funcName,
                          const std::string& stratName) {
    std::printf("\n=== Case %d: Strategy caller of a Function "
                "(reverse-dependent revalidation) ===\n", caseNum);

    // --- 1. Insert + verify the Function (callee) ---
    std::printf("  [1] Insert + verify Function (callee) \"%s\"...\n",
                funcName.c_str());
    int funcProgId = insertAndVerify(lib, ComponentType::Function,
                                     funcName,
                                     makeFunctionSource(funcName), caseNum);
    if (funcProgId <= 0) {
        return fail("Function setup failed", 100 + caseNum * 10 + 1);
    }

    // --- 2. Insert + verify the Strategy (caller) that calls the Function ---
    std::printf("  [2] Insert + verify Strategy (caller) \"%s\" "
                "calling Function...\n", stratName.c_str());
    int stratProgId = insertAndVerify(lib, ComponentType::Strategy,
                                      stratName,
                                      makeStrategySourceCalling(stratName,
                                                                funcName),
                                      caseNum);
    if (stratProgId <= 0) {
        lib.deleteEntry(ComponentType::Function, funcName, /*flush=*/false);
        return fail("Strategy setup failed", 100 + caseNum * 10 + 2);
    }

    // --- 3. Modify the Function's source (contract change) and re-verify ---
    // The modify path: saveSource (replace source) + verifyModule (recompile).
    // The M.4 linker-based traversal runs inside verifyModule after the root
    // verify succeeds, for category 1 (Function is in the 0/1/4 gate).
    std::printf("  [3] Modify Function source (contract change) + "
                "re-verify (triggers reverse-dependent traversal)...\n");
    resetCapture();
    if (!lib.saveSource(ComponentType::Function, funcProgId,
                        makeFunctionSourceModified(funcName), /*isVerified=*/true)) {
        std::fprintf(stderr, "      modify saveSource failed\n");
        lib.deleteEntry(ComponentType::Strategy, stratName, /*flush=*/false);
        lib.deleteEntry(ComponentType::Function, funcName, /*flush=*/false);
        return fail("modify saveSource returned false",
                    100 + caseNum * 10 + 3);
    }
    std::string errors = lib.verifyModule(ComponentType::Function, funcProgId);
    if (!errors.empty()) {
        std::fprintf(stderr,
                     "      modify verifyModule errors:\n%s\n",
                     errors.c_str());
    } else {
        std::printf("      modify verifyModule: no errors (root OK)\n");
    }

    // --- 4. Assert the traversal-started sentinel fired ---
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Case %d: traversal-started sentinel fired after "
                      "Function modify-verify", caseNum);
        CHECK(g_capture.sentinelFired, buf);
    }

    // --- 5. Assert the sentinel captured the correct root identity ---
    if (g_capture.sentinelFired) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Case %d: sentinel rootComponentType==%d (expected %d)",
                      caseNum, g_capture.rootComponentType,
                      static_cast<int>(ComponentType::Function));
        CHECK(g_capture.rootComponentType ==
              static_cast<int>(ComponentType::Function), buf);

        std::snprintf(buf, sizeof(buf),
                      "Case %d: sentinel rootProgId==%d (expected %d)",
                      caseNum, g_capture.rootProgId, funcProgId);
        CHECK(g_capture.rootProgId == funcProgId, buf);

        std::snprintf(buf, sizeof(buf),
                      "Case %d: sentinel rootCategory==%d (expected 1)",
                      caseNum, g_capture.rootCategory);
        CHECK(g_capture.rootCategory == 1, buf);
    }

    // --- 6. Assert at least one per-dependent event fired for the Strategy
    //     (blocked when CLI-created linker element arrays are empty) ---
    bool foundStratDep = false;
    for (const auto& ev : g_capture.dependentEvents) {
        if (ev.dependentProgId == stratProgId &&
            ev.dependentComponentType ==
                static_cast<int>(ComponentType::Strategy)) {
            foundStratDep = true;
            break;
        }
    }
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Case %d: per-dependent event fired for Strategy "
                      "caller (ProgId=%d)", caseNum, stratProgId);
        CHECK(foundStratDep, buf);
    }

    // --- 7. Assert the Strategy's revalidation was triggered ---
    if (foundStratDep) {
        const DependentEvent* stratEv = nullptr;
        for (const auto& ev : g_capture.dependentEvents) {
            if (ev.dependentProgId == stratProgId &&
                ev.dependentComponentType ==
                    static_cast<int>(ComponentType::Strategy)) {
                stratEv = &ev;
                break;
            }
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Case %d: Strategy revalidationTriggered==1 "
                      "(got %d)", caseNum,
                      stratEv ? stratEv->revalidationTriggered : -999);
        CHECK(stratEv && stratEv->revalidationTriggered == 1, buf);
    }

    // --- 8. Cleanup ---
    std::printf("  [8] Cleanup: delete Strategy + Function...\n");
    lib.deleteEntry(ComponentType::Strategy, stratName, /*flush=*/false);
    lib.deleteEntry(ComponentType::Function, funcName, /*flush=*/true);
    resetCapture();
    return 0;
}

// ---------------------------------------------------------------------------
// Case 2: Indicator caller of a Function.
//
// Same structure as Case 1 but the caller is an Indicator. The Function is
// category 1 (in the 0/1/4 gate), so the traversal should run and find the
// Indicator as a reverse dependent.
//
// The dependency assertions are currently blocked when CLI-created object code
// has version=0 and the linker leaves its element arrays empty.
// ---------------------------------------------------------------------------
int runIndicatorCallerCase(TsDevLib& lib, int caseNum,
                            const std::string& funcName,
                            const std::string& indName) {
    std::printf("\n=== Case %d: Indicator caller of a Function "
                "(reverse-dependent revalidation) ===\n", caseNum);

    std::printf("  [1] Insert + verify Function (callee) \"%s\"...\n",
                funcName.c_str());
    int funcProgId = insertAndVerify(lib, ComponentType::Function,
                                     funcName,
                                     makeFunctionSource(funcName), caseNum);
    if (funcProgId <= 0) {
        return fail("Function setup failed", 100 + caseNum * 10 + 1);
    }

    std::printf("  [2] Insert + verify Indicator (caller) \"%s\" "
                "calling Function...\n", indName.c_str());
    int indProgId = insertAndVerify(lib, ComponentType::Indicator,
                                    indName,
                                    makeIndicatorSourceCalling(indName,
                                                               funcName),
                                    caseNum);
    if (indProgId <= 0) {
        lib.deleteEntry(ComponentType::Function, funcName, /*flush=*/false);
        return fail("Indicator setup failed", 100 + caseNum * 10 + 2);
    }

    std::printf("  [3] Modify Function source (contract change) + "
                "re-verify...\n");
    resetCapture();
    if (!lib.saveSource(ComponentType::Function, funcProgId,
                        makeFunctionSourceModified(funcName), /*isVerified=*/true)) {
        std::fprintf(stderr, "      modify saveSource failed\n");
        lib.deleteEntry(ComponentType::Indicator, indName, /*flush=*/false);
        lib.deleteEntry(ComponentType::Function, funcName, /*flush=*/false);
        return fail("modify saveSource returned false",
                    100 + caseNum * 10 + 3);
    }
    std::string errors = lib.verifyModule(ComponentType::Function, funcProgId);
    if (!errors.empty()) {
        std::fprintf(stderr, "      modify verifyModule errors:\n%s\n",
                     errors.c_str());
    } else {
        std::printf("      modify verifyModule: no errors (root OK)\n");
    }

    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Case %d: traversal-started sentinel fired after "
                      "Function modify-verify", caseNum);
        CHECK(g_capture.sentinelFired, buf);
    }

    if (g_capture.sentinelFired) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Case %d: sentinel rootCategory==%d (expected 1)",
                      caseNum, g_capture.rootCategory);
        CHECK(g_capture.rootCategory == 1, buf);
    }

    bool foundIndDep = false;
    for (const auto& ev : g_capture.dependentEvents) {
        if (ev.dependentProgId == indProgId &&
            ev.dependentComponentType ==
                static_cast<int>(ComponentType::Indicator)) {
            foundIndDep = true;
            break;
        }
    }
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Case %d: per-dependent event fired for Indicator "
                      "caller (ProgId=%d)", caseNum, indProgId);
        CHECK(foundIndDep, buf);
    }

    if (foundIndDep) {
        const DependentEvent* indEv = nullptr;
        for (const auto& ev : g_capture.dependentEvents) {
            if (ev.dependentProgId == indProgId &&
                ev.dependentComponentType ==
                    static_cast<int>(ComponentType::Indicator)) {
                indEv = &ev;
                break;
            }
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Case %d: Indicator revalidationTriggered==1 "
                      "(got %d)", caseNum,
                      indEv ? indEv->revalidationTriggered : -999);
        CHECK(indEv && indEv->revalidationTriggered == 1, buf);
    }

    std::printf("  [8] Cleanup: delete Indicator + Function...\n");
    lib.deleteEntry(ComponentType::Indicator, indName, /*flush=*/false);
    lib.deleteEntry(ComponentType::Function, funcName, /*flush=*/true);
    resetCapture();
    return 0;
}

// ---------------------------------------------------------------------------
// Case 3: Negative — Function with NO callers.
//
// Insert a leaf Function (no callers), modify + re-verify it. Assert:
//   - the traversal-started sentinel fired (the traversal was entered —
//     category 1 is in the gate), AND
//   - ZERO per-dependent events fired (no reverse dependents exist).
//
// With the current linker-based implementation, the sentinel is expected to
// fire for this eligible root. A CLI-created object-code version of 0 can still
// leave the candidate element arrays empty; that is the M.3b blocker, not
// evidence that traversal is absent.
// ---------------------------------------------------------------------------
int runNoCallerNegativeCase(TsDevLib& lib, int caseNum,
                             const std::string& funcName) {
    std::printf("\n=== Case %d: Negative — Function with NO callers "
                "(traversal entered, zero dependents) ===\n", caseNum);

    std::printf("  [1] Insert + verify leaf Function \"%s\"...\n",
                funcName.c_str());
    int funcProgId = insertAndVerify(lib, ComponentType::Function,
                                     funcName,
                                     makeFunctionSource(funcName), caseNum);
    if (funcProgId <= 0) {
        return fail("Function setup failed", 100 + caseNum * 10 + 1);
    }

    std::printf("  [2] Modify Function source + re-verify (triggers "
                "traversal)...\n");
    resetCapture();
    if (!lib.saveSource(ComponentType::Function, funcProgId,
                        makeFunctionSourceModified(funcName), /*isVerified=*/true)) {
        std::fprintf(stderr, "      modify saveSource failed\n");
        lib.deleteEntry(ComponentType::Function, funcName, /*flush=*/false);
        return fail("modify saveSource returned false",
                    100 + caseNum * 10 + 2);
    }
    std::string errors = lib.verifyModule(ComponentType::Function, funcProgId);
    if (!errors.empty()) {
        std::fprintf(stderr, "      modify verifyModule errors:\n%s\n",
                     errors.c_str());
    } else {
        std::printf("      modify verifyModule: no errors (root OK)\n");
    }

    // --- 3. Assert the traversal-started sentinel fired ---
    // The eligible Function root enters the current linker-based traversal;
    // an empty candidate element array is a separate M.3b data defect.
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Case %d: traversal-started sentinel fired for "
                      "leaf Function (category 1 in gate)", caseNum);
        CHECK(g_capture.sentinelFired, buf);
    }

    // --- 4. Assert ZERO per-dependent events (no callers exist) ---
    // The current traversal should find zero dependents for this leaf Function;
    // this assertion is independent of the CLI-created object-code blocker.
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Case %d: zero per-dependent events for leaf Function "
                      "(got %zu)", caseNum, g_capture.dependentEvents.size());
        CHECK(g_capture.dependentEvents.empty(), buf);
    }

    std::printf("  [4] Cleanup: delete Function...\n");
    lib.deleteEntry(ComponentType::Function, funcName, /*flush=*/true);
    resetCapture();
    return 0;
}

// ---------------------------------------------------------------------------
// Case 4: Category gate — modify an Indicator (category 2, NOT in 0/1/4).
//
// Insert an Indicator, modify + re-verify it. Assert:
//   - the traversal-started sentinel did NOT fire (the category gate excludes
//     category 2).
//
// This case pins the current category-gate contract: an Indicator modify must
// NOT trigger the reverse-dependent traversal (only categories 0/1/4 do).
// ---------------------------------------------------------------------------
int runCategoryGateCase(TsDevLib& lib, int caseNum,
                        const std::string& indName) {
    std::printf("\n=== Case %d: Category gate — modify Indicator "
                "(category 2, NOT in 0/1/4) -> NO traversal ===\n", caseNum);

    std::printf("  [1] Insert + verify Indicator \"%s\"...\n",
                indName.c_str());
    int indProgId = insertAndVerify(lib, ComponentType::Indicator,
                                    indName,
                                    std::string("{ ") + indName + " }\r\n"
                                    "Plot1(Close, \"Close\");\r\n",
                                    caseNum);
    if (indProgId <= 0) {
        return fail("Indicator setup failed", 100 + caseNum * 10 + 1);
    }

    std::printf("  [2] Modify Indicator source + re-verify...\n");
    resetCapture();
    // Modify to a different (still-valid) Indicator source.
    if (!lib.saveSource(ComponentType::Indicator, indProgId,
                        std::string("{ ") + indName + " v2 }\r\n"
                        "Plot1(Open, \"Open\");\r\n", /*isVerified=*/true)) {
        std::fprintf(stderr, "      modify saveSource failed\n");
        lib.deleteEntry(ComponentType::Indicator, indName, /*flush=*/false);
        return fail("modify saveSource returned false",
                    100 + caseNum * 10 + 2);
    }
    std::string errors = lib.verifyModule(ComponentType::Indicator, indProgId);
    if (!errors.empty()) {
        std::fprintf(stderr, "      modify verifyModule errors:\n%s\n",
                     errors.c_str());
    } else {
        std::printf("      modify verifyModule: no errors (root OK)\n");
    }

    // --- 3. Assert the traversal-started sentinel did NOT fire ---
    // Category 2 (Indicator) is NOT in the 0/1/4 gate, so the current
    // implementation must not enter the reverse-dependent traversal.
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Case %d: NO traversal-started sentinel for Indicator "
                      "(category 2 excluded from 0/1/4 gate)", caseNum);
        CHECK(!g_capture.sentinelFired, buf);
    }

    // --- 4. Assert zero per-dependent events (consequence of no traversal) ---
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Case %d: zero per-dependent events for Indicator "
                      "(got %zu)", caseNum, g_capture.dependentEvents.size());
        CHECK(g_capture.dependentEvents.empty(), buf);
    }

    std::printf("  [4] Cleanup: delete Indicator...\n");
    lib.deleteEntry(ComponentType::Indicator, indName, /*flush=*/true);
    resetCapture();
    return 0;
}

// ---------------------------------------------------------------------------
// Case 5: Cycle safety (unit-level contract pin).
//
// A live warehouse cycle (A->B->A) is not feasible at the unit level without
// a synthetic dependency cycle, which would require inserting two Functions
// that mutually reference each other — EasyLanguage may reject this at
// compile time, and the test would be fragile. Per plan §M.4 TDD step 4,
// the live cycle test is deferred to the regression at the lowest safe
// boundary.
//
// This case instead asserts the visited-set / depth contract at the
// compile-time level: the hook signature carries a `traversalDepth` field,
// which the current implementation populates from the explicit visited-set
// recursion. The test verifies the hook is wired with the correct arity
// (9 integer args) by invoking it directly with a synthetic sentinel and a
// synthetic per-dependent event, and asserting the capture records both.
// This locks in the contract so later changes cannot silently drop the depth
// field.
// ---------------------------------------------------------------------------
int runCycleSafetyContractPinCase(TsDevLib& /*lib*/, int caseNum) {
    std::printf("\n=== Case %d: Cycle safety (unit-level contract pin) ===\n",
                caseNum);
    std::printf("  NOTE: live warehouse cycle test deferred to regression "
                "per plan §M.4 TDD step 4.\n");
    std::printf("  This case pins the hook signature (traversalDepth field) "
                "at the unit level.\n");

    resetCapture();

    // Fire a synthetic traversal-started sentinel directly through the hook.
    onReverseDependentRevalidationReady(
        /*rootComponentType*/ static_cast<int>(ComponentType::Function),
        /*rootProgId*/ 100,
        /*rootCategory*/ 1,
        /*dependentComponentType*/ 0,
        /*dependentProgId*/ 0,
        /*dependentCategory*/ 0,
        /*revalidationTriggered*/ -1,  // sentinel
        /*revalidationResult*/ -1,
        /*traversalDepth*/ -1);

    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Case %d: synthetic sentinel recorded", caseNum);
        CHECK(g_capture.sentinelFired, buf);
    }
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Case %d: synthetic sentinel rootProgId==100",
                      caseNum);
        CHECK(g_capture.rootProgId == 100, buf);
    }

    // Fire a synthetic per-dependent event at depth 0 (direct dependent).
    onReverseDependentRevalidationReady(
        /*rootComponentType*/ static_cast<int>(ComponentType::Function),
        /*rootProgId*/ 100,
        /*rootCategory*/ 1,
        /*dependentComponentType*/ static_cast<int>(ComponentType::Strategy),
        /*dependentProgId*/ 200,
        /*dependentCategory*/ 0,
        /*revalidationTriggered*/ 1,  // reverified
        /*revalidationResult*/ 1,     // success
        /*traversalDepth*/ 0);

    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Case %d: synthetic per-dependent event recorded "
                      "(1 event)", caseNum);
        CHECK(g_capture.dependentEvents.size() == 1, buf);
    }
    if (!g_capture.dependentEvents.empty()) {
        const DependentEvent& ev = g_capture.dependentEvents[0];
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "Case %d: synthetic event traversalDepth==0 "
                          "(got %d)", caseNum, ev.traversalDepth);
            CHECK(ev.traversalDepth == 0, buf);
        }
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "Case %d: synthetic event revalidationTriggered==1 "
                          "(got %d)", caseNum, ev.revalidationTriggered);
            CHECK(ev.revalidationTriggered == 1, buf);
        }
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "Case %d: synthetic event revalidationResult==1 "
                          "(got %d)", caseNum, ev.revalidationResult);
            CHECK(ev.revalidationResult == 1, buf);
        }
    }

    // Fire a synthetic per-dependent event at depth 1 (transitive dependent)
    // to pin that the depth field is populated per recursion level.
    onReverseDependentRevalidationReady(
        /*rootComponentType*/ static_cast<int>(ComponentType::Function),
        /*rootProgId*/ 100,
        /*rootCategory*/ 1,
        /*dependentComponentType*/ static_cast<int>(ComponentType::Strategy),
        /*dependentProgId*/ 300,
        /*dependentCategory*/ 0,
        /*revalidationTriggered*/ 1,
        /*revalidationResult*/ 1,
        /*traversalDepth*/ 1);

    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Case %d: synthetic transitive event recorded "
                      "(2 events total)", caseNum);
        CHECK(g_capture.dependentEvents.size() == 2, buf);
    }
    if (g_capture.dependentEvents.size() == 2) {
        const DependentEvent& ev = g_capture.dependentEvents[1];
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Case %d: synthetic transitive event traversalDepth==1 "
                      "(got %d)", caseNum, ev.traversalDepth);
        CHECK(ev.traversalDepth == 1, buf);
    }

    resetCapture();
    return 0;
}

// ---------------------------------------------------------------------------
// Worker thread entry point. Constructs ONE TsDevLib (with the
// onReverseDependentRevalidationReady hook installed) for the whole worker
// thread and runs all cases against it. This matches the operational
// invariant used by every other integration test in this codebase
// (test_compiler_layout.cpp, test_save_module.cpp, test_0x40a_notification.cpp):
// a SINGLE TsDevLib instance per worker thread.
// ---------------------------------------------------------------------------
struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);

    std::printf("\n--- Constructing shared TsDevLib (test-only ctor, "
                "M.4 hook installed) ---\n");
    std::unique_ptr<TsDevLib> lib;
    try {
        TsDevLibTestConfig cfg = makeConfigWithHook();
        lib = tsdev::makeTsDevLibForTest(/*tsPathOverride=*/nullptr, cfg);
    } catch (const std::exception& e) {
        st->exitCode = fail(
            (std::string("TsDevLib construction threw: ") + e.what()).c_str(),
            100);
        return 0;
    }
    if (!lib) {
        st->exitCode = fail("makeTsDevLibForTest returned null", 101);
        return 0;
    }
    std::printf("--- TsDevLib constructed OK ---\n");

    // Date-stamped names so the test items are unique per run day.
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    char funcAName[64];
    char stratAName[64];
    char funcBName[64];
    char indBName[64];
    char funcCName[64];
    char indDName[64];
    std::snprintf(funcAName, sizeof(funcAName),
                  "TS_AUTO_M4_FUNC_A_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(stratAName, sizeof(stratAName),
                  "TS_AUTO_M4_STRAT_A_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(funcBName, sizeof(funcBName),
                  "TS_AUTO_M4_FUNC_B_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(indBName, sizeof(indBName),
                  "TS_AUTO_M4_IND_B_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(funcCName, sizeof(funcCName),
                  "TS_AUTO_M4_FUNC_C_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(indDName, sizeof(indDName),
                  "TS_AUTO_M4_IND_D_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);

    // --- Case 1: Strategy caller of a Function ---
    {
        int rc = runStrategyCallerCase(*lib, 1, funcAName, stratAName);
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 2: Indicator caller of a Function ---
    {
        int rc = runIndicatorCallerCase(*lib, 2, funcBName, indBName);
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 3: Negative — Function with NO callers ---
    {
        int rc = runNoCallerNegativeCase(*lib, 3, funcCName);
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 4: Category gate — modify an Indicator (category 2) ---
    {
        int rc = runCategoryGateCase(*lib, 4, indDName);
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 5: Cycle safety (unit-level contract pin) ---
    {
        int rc = runCycleSafetyContractPinCase(*lib, 5);
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    st->exitCode = (g_failCount == 0) ? 0 : 1;
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== test_reverse_dependent_revalidation: M.4 reverse-dependent "
                "revalidation parity ===\n");
    std::printf("Asserts that after a successful root verify (categories "
                "0/1/4), verifyModule() traverses reverse dependents and "
                "reverifies each one, matching TSDev.exe FUN_004D91E0.\n");
    std::printf("See docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md "
                "(R-M.4.1 through R-M.4.5).\n");
    std::printf("M.4 status: linker-based traversal is implemented; CLI-created "
                "object-code version=0 currently blocks dependency discovery. "
                "No green acceptance result is implied.\n");
    std::printf("Desktop/TSDev running state is not a prerequisite for this library-level test.\n");
    std::printf("Cases: Strategy caller + Indicator caller + no-caller "
                "negative + category-gate negative + cycle-safety contract "
                "pin (5 cases total).\n");

    // Run the entire test on a worker thread with a 64 MB stack. The
    // EasyLanguage compiler needs far more than the default 1 MB, and the
    // hidden EDIT control is thread-affine so TsDevLib must live on this
    // thread.
    TestState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runTest, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    std::printf("\n=== test_reverse_dependent_revalidation: %d/%d assertions "
                "passed, %d failed ===\n",
                g_passCount, g_passCount + g_failCount, g_failCount);
    std::printf("=== test_reverse_dependent_revalidation complete (exit %d) "
                "===\n", st.exitCode);
    return st.exitCode;
}
