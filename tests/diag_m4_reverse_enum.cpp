// diag_m4_reverse_enum.cpp
//
// DIAGNOSTIC ONLY (not part of the regression suite). Read-only with respect
// to production LOGIC: the only production-file change required to run this
// diagnostic is a single behavior-preserving `friend struct
// DiagM4ReverseEnumAccess;` declaration in src/tsdev_lib.h, which grants name
// access to the private enumerateReverseDependents method and the private
// ReverseDependent nested struct. No production behavior is altered.
//
// Purpose: Verify the new linker-based enumerateReverseDependents
// implementation in src/tsdev_lib.cpp finds reverse dependents for EXISTING
// items, by calling the ACTUAL production method directly (not a re-impl).
//
// Background (confirmed by the prior diag_m4_deplist_dump diagnostic):
//   - Indicator ProgId=3  has 0xfa3 entries with funcRefs 516, 517
//                          (Functions 516 and 517)
//   - Indicator ProgId=8  has 0xfa3 entries with funcRefs 21, 458, 516, 517
//
// Therefore:
//   enumerateReverseDependents(516, 4) -> should find Indicators 3 and 8
//   enumerateReverseDependents(517, 4) -> should find Indicators 3 and 8
//   enumerateReverseDependents(21,  4) -> should find Indicator 8
//   enumerateReverseDependents(458, 4) -> should find Indicator 8
//   enumerateReverseDependents(999999, 4) -> negative control, should return 0
//
// The second arg (rootCategory) is currently unused by the implementation
// (it scans all 8 categories); we pass 4 (Function) per the task spec.
//
// Flow:
//   1. Print PID and pre-check TSCrashReport.exe.
//   2. Construct a TsDevLib (test-only ctor, real ComApi, no hooks) on a
//      64 MB-stack worker thread (same pattern as diag_m4_deplist_dump).
//   3. For each (rootProgId, expectedProgIds) probe, call the ACTUAL
//      lib.enumerateReverseDependents(rootProgId, 4) via the friend accessor,
//      time the call, print the results, and verify the expected ProgIds are
//      present.
//   4. Negative control: enumerateReverseDependents(999999, 4) must return 0.
//   5. Check for TSCrashReport.exe after all operations.
//
// READ-ONLY: does NOT insert/modify/delete any items. Only calls the
// read-only enumerateReverseDependents method (which itself only reads
// warehouse records via the linker + Ordinal_28/70 + _LoadMasterRecsEX@16).
//
// TIMING: each call iterates ALL items in ALL 8 categories and calls the
// linker for each. May take 30-60s per call; total 3-5 min. A 600s hard
// timeout is applied.

#include "../src/tsdev_lib_test_hooks.h"  // internal/test-only header (NOT public)
#include "tsdev_lib.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

// -------------------------------------------------------------------------
// DiagReverseDependent — PUBLIC POD mirror of the private
// TsDevLib::ReverseDependent. We use this OUTSIDE the friend scope (in
// ProbeResult, printing, verification) so non-friend code never has to name
// the private nested struct. The friend accessor converts the private vector
// to this POD vector before returning it across the friend boundary.
// -------------------------------------------------------------------------
struct DiagReverseDependent {
    int progId = 0;
    int category = 0;
    int subtype = 0;
};

// -------------------------------------------------------------------------
// DiagM4ReverseEnumAccess — test-only friend accessor.
//
// Declared as a friend in src/tsdev_lib.h (behavior-preserving test seam).
// Lives at namespace tsdev scope (NOT in an anonymous namespace) so the
// unqualified `friend struct DiagM4ReverseEnumAccess;` declaration inside
// tsdev::TsDevLib resolves to THIS type. An anonymous-namespace definition
// would give the type internal linkage and a distinct identity from the
// befriended name, defeating friend access.
//
// Calls the private TsDevLib::enumerateReverseDependents and converts the
// private ReverseDependent vector to a public DiagReverseDependent POD
// vector before returning, so non-friend code never names the private type.
// -------------------------------------------------------------------------
namespace tsdev {
struct DiagM4ReverseEnumAccess {
    static std::vector<DiagReverseDependent> enumerate(
        TsDevLib& lib, int rootProgId, int rootCategory) {
        std::vector<TsDevLib::ReverseDependent> priv =
            lib.enumerateReverseDependents(rootProgId, rootCategory);
        std::vector<DiagReverseDependent> out;
        out.reserve(priv.size());
        for (const auto& d : priv) {
            out.push_back({d.progId, d.category, d.subtype});
        }
        return out;
    }
};
} // namespace tsdev

using namespace tsdev;

namespace {

// -------------------------------------------------------------------------
// Real ComApi — binds the actual CoInitializeEx / CoUninitialize so the
// test-only ctor enters a real STA apartment (required for orlib20 / OLE).
// (Same pattern as diag_m4_deplist_dump / test_reverse_dependent_revalidation.)
// -------------------------------------------------------------------------
ComApi realComApi() {
    return ComApi{&CoInitializeEx, &CoUninitialize};
}

// -------------------------------------------------------------------------
// Result of a single probe.
// -------------------------------------------------------------------------
struct ProbeResult {
    int rootProgId = 0;
    int rootCategory = 0;
    double elapsedMs = 0.0;
    int foundCount = 0;
    std::vector<DiagReverseDependent> dependents;
    bool sehOrError = false;
};

// -------------------------------------------------------------------------
// Run one probe: call the ACTUAL enumerateReverseDependents and time it.
//
// SEH note: enumerateReverseDependents is INTERNALLY fully SEH-protected
// (every orlib20 call site inside it is wrapped in __try/__except, per
// src/tsdev_lib.cpp). We do NOT add an outer __try here because (a) the
// production method already swallows SEH internally, and (b) wrapping a
// function that returns a std::vector (which requires object unwinding) in
// __try is rejected by MSVC as C2712. We use a C++ try/catch only as a
// last-resort guard against a std::bad_alloc from the vector copy.
// -------------------------------------------------------------------------
ProbeResult runProbe(TsDevLib& lib, int rootProgId, int rootCategory,
                     const char* label) {
    ProbeResult pr;
    pr.rootProgId = rootProgId;
    pr.rootCategory = rootCategory;

    std::printf("\n>>> Probe: enumerateReverseDependents(rootProgId=%d, "
                "rootCategory=%d) [%s] <<<\n",
                rootProgId, rootCategory, label);

    auto t0 = std::chrono::high_resolution_clock::now();
    try {
        pr.dependents = DiagM4ReverseEnumAccess::enumerate(lib, rootProgId,
                                                           rootCategory);
        pr.foundCount = static_cast<int>(pr.dependents.size());
    } catch (const std::exception& e) {
        pr.sehOrError = true;
        std::fprintf(stderr, "    EXCEPTION during call: %s\n", e.what());
    } catch (...) {
        pr.sehOrError = true;
        std::fprintf(stderr, "    UNKNOWN EXCEPTION during call\n");
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    pr.elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::printf("    call %s in %.2f ms, found %d reverse dependent(s)%s\n",
                pr.sehOrError ? "ERROR/EXC" : "OK",
                pr.elapsedMs, pr.foundCount,
                pr.sehOrError ? " [EXC]" : "");

    // Print each dependent.
    for (const auto& d : pr.dependents) {
        const char* catLabel = "Unknown";
        switch (d.category) {
            case 0: catLabel = "Strategy"; break;
            case 1: catLabel = "Function?"; break;
            case 2: catLabel = "Indicator"; break;
            case 3: catLabel = "ActivityBar"; break;
            case 4: catLabel = "Function"; break;
            case 5: catLabel = "PaintBar?"; break;
            case 6: catLabel = "ProbabilityMap?"; break;
            case 7: catLabel = "ShowMe?"; break;
            default: break;
        }
        std::printf("    -> ProgId=%d category=%d (%s) subtype=%d\n",
                    d.progId, d.category, catLabel, d.subtype);
    }
    if (pr.dependents.empty()) {
        std::printf("    -> (no reverse dependents found)\n");
    }
    return pr;
}

// -------------------------------------------------------------------------
// Verify that a set of expected ProgIds is present in the probe result.
// Returns the number of missing expected ProgIds (0 = all present).
// -------------------------------------------------------------------------
int verifyExpected(const ProbeResult& pr, const std::vector<int>& expected,
                   const char* label) {
    int missing = 0;
    for (int exp : expected) {
        bool found = false;
        for (const auto& d : pr.dependents) {
            if (d.progId == exp) { found = true; break; }
        }
        if (found) {
            std::printf("  [VERIFY %s] ProgId=%d PRESENT in reverse "
                        "dependents\n", label, exp);
        } else {
            std::printf("  [VERIFY %s] ProgId=%d MISSING from reverse "
                        "dependents\n", label, exp);
            missing++;
        }
    }
    return missing;
}

// -------------------------------------------------------------------------
// Diagnostic state.
// -------------------------------------------------------------------------
struct DiagState {
    int exitCode = 0;
    int totalMissing = 0;       // expected ProgIds not found across probes
    int totalSeh = 0;           // probes that raised SEH
    int negativeControlFailed = 0;
};

DWORD WINAPI runDiag(LPVOID param) {
    DiagState* st = static_cast<DiagState*>(param);

    std::printf("=== diag_m4_reverse_enum: verify linker-based "
                "enumerateReverseDependents on existing items ===\n");
    std::printf("Purpose: Call the ACTUAL production "
                "enumerateReverseDependents and verify it finds\n");
    std::printf("  Indicators 3 and 8 as reverse dependents of Functions "
                "516/517/21/458.\n");
    std::printf("  READ-ONLY — no insert/modify/delete.\n");
    std::printf("  PID=%lu\n", GetCurrentProcessId());

    // --- Step 1: construct a real TsDevLib (full init). No hooks. ---
    std::printf("\n[1] Constructing TsDevLib (test-only ctor, real ComApi, "
                "no hooks)...\n");
    std::unique_ptr<TsDevLib> lib;
    try {
        TsDevLibTestConfig cfg;
        cfg.comApi        = realComApi();
        cfg.throwAfterCom = false;
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

    // --- Step 2: run the probes against the ACTUAL method ---
    std::printf("\n[2] Calling enumerateReverseDependents (ACTUAL production "
                "method)...\n");

    // Probe A: rootProgId=516 (Function) -> expect Indicators 3 and 8.
    ProbeResult pa = runProbe(*lib, /*rootProgId=*/516, /*rootCategory=*/4,
                              "Function 516");
    st->totalSeh += pa.sehOrError ? 1 : 0;
    st->totalMissing += verifyExpected(pa, {3, 8}, "516->Ind3,Ind8");

    // Probe B: rootProgId=517 (Function) -> expect Indicators 3 and 8.
    ProbeResult pb = runProbe(*lib, /*rootProgId=*/517, /*rootCategory=*/4,
                              "Function 517");
    st->totalSeh += pb.sehOrError ? 1 : 0;
    st->totalMissing += verifyExpected(pb, {3, 8}, "517->Ind3,Ind8");

    // Probe C: rootProgId=21 (Function) -> expect Indicator 8.
    ProbeResult pc = runProbe(*lib, /*rootProgId=*/21, /*rootCategory=*/4,
                              "Function 21");
    st->totalSeh += pc.sehOrError ? 1 : 0;
    st->totalMissing += verifyExpected(pc, {8}, "21->Ind8");

    // Probe D: rootProgId=458 (Function) -> expect Indicator 8.
    ProbeResult pd = runProbe(*lib, /*rootProgId=*/458, /*rootCategory=*/4,
                              "Function 458");
    st->totalSeh += pd.sehOrError ? 1 : 0;
    st->totalMissing += verifyExpected(pd, {8}, "458->Ind8");

    // Probe E (negative control): rootProgId=999999 -> expect ZERO results.
    ProbeResult pe = runProbe(*lib, /*rootProgId=*/999999,
                              /*rootCategory=*/4, "Negative control 999999");
    st->totalSeh += pe.sehOrError ? 1 : 0;
    if (pe.foundCount != 0) {
        std::printf("  [VERIFY neg-control] FAIL: expected 0 results, got "
                    "%d\n", pe.foundCount);
        st->negativeControlFailed++;
    } else {
        std::printf("  [VERIFY neg-control] PASS: 0 results (as expected)\n");
    }

    // --- Final summary ---
    std::printf("\n=== FINAL SUMMARY ===\n");
    auto printProbe = [](const char* label, const ProbeResult& pr) {
        std::printf("  %-28s rootProgId=%-7d found=%-3d elapsed=%.0f ms%s\n",
                    label, pr.rootProgId, pr.foundCount, pr.elapsedMs,
                    pr.sehOrError ? " [SEH]" : "");
    };
    printProbe("Probe A (516->3,8)", pa);
    printProbe("Probe B (517->3,8)", pb);
    printProbe("Probe C (21->8)",    pc);
    printProbe("Probe D (458->8)",   pd);
    printProbe("Probe E (999999->0)", pe);

    double totalMs = pa.elapsedMs + pb.elapsedMs + pc.elapsedMs +
                     pd.elapsedMs + pe.elapsedMs;
    std::printf("\n  Total elapsed across all probes: %.0f ms (%.2f s)\n",
                totalMs, totalMs / 1000.0);
    std::printf("  Total expected ProgIds MISSING: %d\n", st->totalMissing);
    std::printf("  Total probes that raised SEH: %d\n", st->totalSeh);
    std::printf("  Negative control failures: %d\n",
                st->negativeControlFailed);

    // Overall pass/fail.
    bool pass = (st->totalMissing == 0) && (st->totalSeh == 0) &&
                (st->negativeControlFailed == 0);
    std::printf("\n  OVERALL: %s\n", pass ? "PASS" : "FAIL");
    st->exitCode = pass ? 0 : 1;

    std::printf("\n=== DIAG COMPLETE ===\n");
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Pre-check: is TSCrashReport.exe already running?
    std::printf("[PRE] Checking for TSCrashReport.exe...\n");
    system("tasklist /FI \"IMAGENAME eq TSCrashReport.exe\" 2>NUL | "
           "findstr /I TSCrashReport && echo [PRE] TSCrashReport.exe IS "
           "running || echo [PRE] TSCrashReport.exe NOT running");

    // Run on a 64 MB-stack worker thread (TsDevLib is thread-affine and
    // orlib20 calls need a large stack).
    DiagState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runDiag, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    // 600-second hard timeout — 5 probes, each iterating all 8 categories
    // with linker calls per record, may take several minutes total.
    DWORD wait = WaitForSingleObject(hThread, 600000);
    if (wait == WAIT_TIMEOUT) {
        std::fprintf(stderr, "\nFATAL: diagnostic timed out after 600s, "
                     "terminating.\n");
        TerminateThread(hThread, 99);
        system("taskkill /F /IM TSCrashReport.exe >NUL 2>&1");
        CloseHandle(hThread);
        return 98;
    }
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);

    // Post-check: did TSCrashReport.exe spawn (indicating a crash)?
    std::printf("\n[POST] Checking for TSCrashReport.exe...\n");
    system("tasklist /FI \"IMAGENAME eq TSCrashReport.exe\" 2>NUL | "
           "findstr /I TSCrashReport && echo [POST] TSCrashReport.exe IS "
           "running (crash occurred) || echo [POST] TSCrashReport.exe NOT "
           "running (no crash)");

    return st.exitCode;
}
