// diag_m4_enum_speed.cpp
//
// DIAGNOSTIC ONLY (not part of the regression suite).
//
// Historical diagnostic purpose: measure the rejected GetDependencies-based
// M.4 enumeration approach, which was observed to be too slow (the M.4
// integration test hung for 10+ minutes during Case 1). This is not the
// current production path.
//
// The current enumerateReverseDependents implementation uses the linker
// (ordinal 85) and linked per-element dep-lists. The historical approach below
// iterates all 8 categories (0-7), loads all master records for each category
// via _LoadMasterRecsEX@16, and for EACH record calls GetDependencies for EACH
// element index (0..elemCount-1).
//
// This historical diagnostic:
//   1. Constructs a real TsDevLib via the test-only ctor (real ComApi, no
//      hooks) on a 64 MB-stack worker thread.
//   2. Constructs a SEPARATE OrlibResolver on the same orlib20.dll to resolve
//      the old measurement exports: _LoadMasterRecsEX@16 and GetDependencies.
//   3. For each category 0-7, calls _LoadMasterRecsEX@16 (filterMode=1) and
//      prints the record count.
//   4. For the first 5 records of each category, reads the element count at
//      +0x29 and prints it.
//   5. Times a single GetDependencies call (element 0 of the first record of
//      the first non-empty category).
//   6. Prints a summary of that historical API path. These measurements are
//      unverified historical/transient diagnostics, not M.4 acceptance
//      evidence.
//
// NOTE on refreshCache(): enumerateReverseDependents calls refreshCache() first.
// refreshCache() is a private method and cannot be called from this diagnostic
// without modifying production headers. refreshCache() dispatches only two
// commands (0x0F + 0x4E) and is fast; it makes newly-created in-session
// records visible to _LoadMasterRecsEX but does not affect the per-record /
// per-element cost of the historical API path measured here. Any timing
// numbers produced by this diagnostic are unverified historical/transient
// diagnostics, not current linker-path timings or M.4 acceptance evidence.
//
// This diagnostic does NOT modify any production code and does NOT add debug
// code to production files. Its 30-second limit is an operational timeout, not
// a measured completion time.

#include "../src/orlib_resolver.h"   // OrlibResolver (public)
#include "../src/tsdev_lib_test_hooks.h"  // internal/test-only header (NOT public)
#include "tsdev_lib.h"
#include "process_inspection.h"

#include <chrono>
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

// --- SEH-protected call wrappers (replicate the POD-only wrappers in
// tsdev_lib.cpp so we can call the resolved exports without touching
// production code). POD-only so __try/__except is legal under MSVC (C2712). ---

typedef int (__stdcall *LoadMasterRecsExFn)(int category, HGLOBAL* outBuf,
                                            const char* path, int filterMode);
typedef HGLOBAL (__stdcall *GetDependenciesFn)(int whType, int progId,
                                                int elementIndex,
                                                int* outDepCount);

struct LoadMasterRecsExSehResult { int result; };
bool callLoadMasterRecsEx(LoadMasterRecsExFn fn, int category, HGLOBAL* outBuf,
                          const char* path, int filterMode,
                          LoadMasterRecsExSehResult* out) {
    out->result = -1;
    __try {
        out->result = fn(category, outBuf, path, filterMode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

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

struct DiagState {
    int exitCode = 0;
};

// Per-category record count + element counts for the first 5 records.
struct CatInfo {
    int category;
    int recordCount;        // from _LoadMasterRecsEX (result)
    bool loadSeh;           // SEH during _LoadMasterRecsEX
    std::vector<int> first5ElemCounts;
    int firstProgId;        // ProgId of first record (for the GetDeps timing)
    int firstSubType;       // subType of first record
    int firstElemCount;     // elemCount of first record
};

DWORD WINAPI runDiag(LPVOID param) {
    DiagState* st = static_cast<DiagState*>(param);

    std::printf("=== diag_m4_enum_speed: enumerateReverseDependents speed "
                "diagnostic ===\n");

    // --- Step 1: construct a real TsDevLib (full init: patches, OrlibInit,
    // EDIT control). No hooks. ---
    std::printf("\n[1] Constructing TsDevLib (test-only ctor, real ComApi, "
                "no hooks)...\n");
    std::unique_ptr<TsDevLib> lib;
    try {
        TsDevLibTestConfig cfg;
        cfg.comApi        = realComApi();
        cfg.throwAfterCom = false;
        // All hooks left nullptr — we do not drive any verify/delete here.
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

    // --- Step 2: resolve the two exports via a separate OrlibResolver.
    // orlib20.dll is already loaded + patched + OrlibInit'd by TsDevLib.
    // LoadLibraryW(L"orlib20.dll") just bumps the refcount on the same image,
    // so the patched code and initialized globals are shared. ---
    std::printf("\n[2] Resolving _LoadMasterRecsEX@16 and GetDependencies via "
                "OrlibResolver...\n");
    LoadMasterRecsExFn  loadMasterRecsEx = nullptr;
    GetDependenciesFn   getDependencies  = nullptr;
    try {
        OrlibResolver resolver(L"orlib20.dll");
        loadMasterRecsEx = reinterpret_cast<LoadMasterRecsExFn>(
            resolver.resolveExport("_LoadMasterRecsEX@16"));
        getDependencies  = reinterpret_cast<GetDependenciesFn>(
            resolver.resolveExport("GetDependencies"));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: OrlibResolver/resolveExport threw: %s\n",
                     e.what());
        st->exitCode = 102;
        return 0;
    }
    if (!loadMasterRecsEx || !getDependencies) {
        std::fprintf(stderr, "FATAL: failed to resolve exports "
                     "(loadMasterRecsEx=%p getDependencies=%p)\n",
                     loadMasterRecsEx, getDependencies);
        st->exitCode = 103;
        return 0;
    }
    std::printf("    Resolved OK: _LoadMasterRecsEX@16=%p GetDependencies=%p\n",
                loadMasterRecsEx, getDependencies);

    // --- Step 3 & 4: enumerate all 8 categories. ---
    // Constants matching enumerateReverseDependents in src/tsdev_lib.cpp.
    const int kRecordSize    = 0x10A;
    const int kProgIdOffset  = 0x00;   // short ProgId
    const int kSubTypeOffset = 0xE9;   // byte subtype
    const int kElemCountOff  = 0x29;   // int element count

    std::printf("\n[3] Enumerating categories 0-7 via _LoadMasterRecsEX@16 "
                "(filterMode=1)...\n");
    std::printf("    cat | count | first5 elemCounts (ProgId,subType)\n");
    std::printf("    ----+-------+----------------------------------\n");

    std::vector<CatInfo> cats;
    int totalRecords = 0;
    int maxElemCount = 0;
    int minElemCount = INT_MAX;
    int elemCountSum = 0;
    int elemCountSamples = 0;

    for (int cat = 0; cat <= 7; cat++) {
        CatInfo info;
        info.category    = cat;
        info.recordCount = 0;
        info.loadSeh     = false;
        info.firstProgId = 0;
        info.firstSubType = 0;
        info.firstElemCount = 0;

        HGLOBAL buf = NULL;
        LoadMasterRecsExSehResult lmrSeh;
        if (!callLoadMasterRecsEx(loadMasterRecsEx, cat, &buf,
                                  (const char*)NULL, 1, &lmrSeh)) {
            info.loadSeh = true;
            cats.push_back(info);
            std::printf("    %3d | SEH   | (load raised)\n", cat);
            continue;
        }
        int count = lmrSeh.result;
        info.recordCount = count;
        if (count <= 0 || buf == NULL) {
            if (buf) GlobalFree(buf);
            cats.push_back(info);
            std::printf("    %3d | %5d | (empty)\n", cat, count);
            continue;
        }
        totalRecords += count;

        char* records = (char*)GlobalLock(buf);
        if (!records) {
            GlobalFree(buf);
            info.recordCount = -1;
            cats.push_back(info);
            std::printf("    %3d | %5d | (GlobalLock failed)\n", cat, count);
            continue;
        }

        // First 5 records: read elemCount (+0x29), progId (+0x00), subType (+0xE9).
        int toRead = (count < 5) ? count : 5;
        for (int i = 0; i < toRead; i++) {
            char* rec = records + i * kRecordSize;
            int ec = *(int*)(rec + kElemCountOff);
            short pid = *(short*)(rec + kProgIdOffset);
            unsigned char sub = (unsigned char)rec[kSubTypeOffset];
            info.first5ElemCounts.push_back(ec);
            if (ec > maxElemCount) maxElemCount = ec;
            if (ec < minElemCount) minElemCount = ec;
            elemCountSum += ec;
            elemCountSamples++;
            if (i == 0) {
                info.firstProgId    = (int)pid;
                info.firstSubType   = (int)sub;
                info.firstElemCount = ec;
            }
        }

        GlobalUnlock(buf);
        GlobalFree(buf);

        cats.push_back(info);

        // Print row.
        std::printf("    %3d | %5d | ", cat, count);
        for (size_t i = 0; i < info.first5ElemCounts.size(); i++) {
            if (i == 0) {
                std::printf("[%d (pid=%d,sub=%d)]",
                            info.first5ElemCounts[i],
                            info.firstProgId, info.firstSubType);
            } else {
                std::printf(" %d", info.first5ElemCounts[i]);
            }
        }
        std::printf("\n");
    }

    // --- Step 5: time a single GetDependencies call.
    // Use the first non-empty category's first record, element 0. ---
    std::printf("\n[4] Timing a single GetDependencies call (element 0 of the "
                "first non-empty category's first record)...\n");
    int timingCat = -1;
    int timingProgId = 0;
    int timingWhType = 0;
    int timingElemCount = 0;
    for (const auto& c : cats) {
        if (c.recordCount > 0 && c.firstProgId > 0) {
            timingCat       = c.category;
            timingProgId    = c.firstProgId;
            timingWhType    = c.category | ((int)c.firstSubType << 8);
            timingElemCount = c.firstElemCount;
            break;
        }
    }

    double oneCallMs = 0.0;
    int oneCallDepCount = -1;
    bool oneCallSeh = false;
    if (timingCat < 0) {
        std::printf("    No non-empty category with a valid ProgId found -- "
                    "cannot time GetDependencies.\n");
    } else {
        std::printf("    Using cat=%d progId=%d whType=0x%X elemCount=%d\n",
                    timingCat, timingProgId, timingWhType, timingElemCount);
        GetDependenciesSehResult depSeh;
        auto t0 = std::chrono::high_resolution_clock::now();
        bool ok = callGetDependencies(getDependencies, timingWhType,
                                      timingProgId, 0, &depSeh);
        auto t1 = std::chrono::high_resolution_clock::now();
        oneCallMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (!ok) {
            oneCallSeh = true;
            std::printf("    GetDependencies raised SEH.\n");
        } else {
            oneCallDepCount = depSeh.depCount;
            std::printf("    GetDependencies returned depCount=%d handle=%p\n",
                        depSeh.depCount, depSeh.handle);
            if (depSeh.handle) {
                GlobalFree(depSeh.handle);
            }
        }
        std::printf("    One call time: %.4f ms\n", oneCallMs);
    }

    // --- Step 6: summary. ---
    double avgElem = (elemCountSamples > 0)
                     ? (double)elemCountSum / elemCountSamples : 0.0;

    std::printf("\n=== SUMMARY ===\n");
    std::printf("Total records across all 8 categories: %d\n", totalRecords);
    std::printf("Element count (first 5 records per category): "
                "min=%d max=%d avg=%.2f (samples=%d)\n",
                (minElemCount == INT_MAX ? 0 : minElemCount),
                maxElemCount, avgElem, elemCountSamples);
    std::printf("One GetDependencies call: %.4f ms (depCount=%d, seh=%d)\n",
                oneCallMs, oneCallDepCount, oneCallSeh ? 1 : 0);

    // Estimate: enumerateReverseDependents calls GetDependencies once per
    // element per record (early-exit when a match is found). Worst case
    // (no match anywhere) = sum over all records of elemCount calls.
    // We only sampled first 5 records per category, so we cannot compute the
    // exact total element count; we estimate using the per-category average
    // of the sampled records, scaled to the full record count.
    long long estTotalElemCalls = 0;
    for (const auto& c : cats) {
        if (c.recordCount <= 0 || c.first5ElemCounts.empty()) continue;
        long long sum = 0;
        for (int ec : c.first5ElemCounts) sum += ec;
        double avg = (double)sum / c.first5ElemCounts.size();
        estTotalElemCalls += (long long)(avg * c.recordCount);
    }
    double estTotalMs = estTotalElemCalls * oneCallMs;
    std::printf("Estimated total GetDependencies calls (worst case, "
                "no early-exit): %lld\n", estTotalElemCalls);
    std::printf("Estimated total enumeration time (worst case): "
                "%.1f ms = %.2f sec = %.2f min\n",
                estTotalMs, estTotalMs / 1000.0,
                estTotalMs / 60000.0);

    std::printf("\n=== DIAGNOSIS ===\n");
    if (totalRecords > 5000) {
        std::printf("  - HIGH record count (%d) is a likely contributor.\n",
                    totalRecords);
    } else {
        std::printf("  - Record count (%d) is moderate; not the primary "
                    "bottleneck.\n", totalRecords);
    }
    if (maxElemCount > 50) {
        std::printf("  - HIGH max element count (%d) multiplies GetDependencies "
                    "calls per record.\n", maxElemCount);
    }
    if (oneCallMs > 1.0) {
        std::printf("  - SLOW GetDependencies (%.4f ms/call) is a likely "
                    "primary bottleneck.\n", oneCallMs);
    } else if (oneCallMs > 0.1) {
        std::printf("  - GetDependencies is moderately slow (%.4f ms/call); "
                    "combined with many calls this adds up.\n", oneCallMs);
    } else {
        std::printf("  - GetDependencies is fast (%.4f ms/call); slowness is "
                    "from the sheer number of calls.\n", oneCallMs);
    }

    st->exitCode = 0;
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

    // Run on a 64 MB-stack worker thread (EasyLanguage compiler / hidden EDIT
    // control need this, and TsDevLib is thread-affine).
    DiagState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runDiag, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    // 30-second hard timeout — report a hang without terminating the worker.
    DWORD wait = WaitForSingleObject(hThread, 30000);
    if (wait == WAIT_TIMEOUT) {
        std::fprintf(stderr,
                     "\nFATAL: diagnostic timed out after 30s; worker was "
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
