// test_delete_diag.cpp — deterministic DELETE diagnostic test harness
//
// Verifies the DELETE flow end-to-end with full pre/post checks:
//   PRE:   Check for stale TSDev.exe/WHServer.exe/test processes (report,
//          do NOT kill).
//   SETUP: Ensure test component (zzz_cli_delete_test, Strategy) exists;
//          INSERT it (createEntry + saveSource + verifyModule) if absent.
//   RUN:   Capture MWRKArea.wrk state, deleteEntry, destroy TsDevLib
//          (OrlibExit persists the deletion).
//   VERIFY:
//          - MWRKArea.wrk timestamp or size changed.
//          - Fresh FindExistPos returns <= 0 (entry gone).
//          - No .ell file remains on disk.
//   POST:  Check for WerFault/crash processes.
//   EXIT:  0 only if ALL checks pass; nonzero on any failure.
//
// Runs on a 64 MB-stack worker thread (the EasyLanguage compiler and the
// thread-affine hidden EDIT control require it), matching the pattern of
// test_insert.cpp and test_delete.cpp. An SEH wrapper around the worker
// thread body catches any unhandled structured exception (e.g., from the
// inline-asm calls inside deleteEntry) and converts it to a nonzero exit
// code instead of crashing the process.

#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>
#include <tlhelp32.h>

using namespace tsdev;

namespace {

// ---------------------------------------------------------------------------
// Test fixture constants
// ---------------------------------------------------------------------------

const char* kTestName = "zzz_cli_delete_test";

// Minimal valid EasyLanguage strategy source. The leading comment names the
// strategy (TSDev convention); the body issues a simple market buy order.
const char* kTestSource =
    "{ zzz_cli_delete_test }\r\n"
    "Buy (\"EnterLong\") next bar at Market;\r\n";

const char* kWrkPath =
    "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\MWRKArea.wrk";

// ---------------------------------------------------------------------------
// Process scanning
// ---------------------------------------------------------------------------

// Scan running processes for the given names. Reports findings to stderr.
// If excludeSelf is true, skips the current process.
// Returns the number of matching processes found (0 = none), or -1 on error.
int scanProcesses(const char* when, const char* const* names, int nameCount,
                  bool excludeSelf) {
    std::fprintf(stderr, "=== Process scan [%s] ===\n", when);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "  CreateToolhelp32Snapshot failed (err=%lu)\n",
                     GetLastError());
        return -1;
    }
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    DWORD selfPid = GetCurrentProcessId();
    int found = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (excludeSelf && pe.th32ProcessID == selfPid)
                continue;
            for (int i = 0; i < nameCount; i++) {
                if (_stricmp(pe.szExeFile, names[i]) == 0) {
                    std::fprintf(stderr, "  FOUND %-22s PID=%lu\n",
                                 names[i], pe.th32ProcessID);
                    ++found;
                    break;
                }
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    if (found == 0) {
        std::fprintf(stderr, "  (none of the target processes found)\n");
    }
    return found;
}

// Pre-run: check for TSDev.exe, WHServer.exe, stale test processes.
// Reports findings but does NOT kill anything. Informational only — does
// not affect the pass/fail exit code.
void preRunProcessCheck() {
    const char* names[] = {
        "TSDev.exe",
        "WHServer.exe",
        "test_delete_diag.exe"
    };
    int found = scanProcesses("pre-run", names, 3, true);
    if (found > 0) {
        std::fprintf(stderr,
                     "  WARNING: Stale/conflicting processes found. "
                     "Not killing them.\n");
    }
}

// Post-run: check for WerFault and stale test processes.
// Returns true if no crash/stale processes found.
bool postRunCrashCheck() {
    const char* names[] = {
        "WerFault.exe",
        "test_delete_diag.exe"  // stale instances of ourselves
    };
    int found = scanProcesses("post-run", names, 2, true);
    if (found > 0) {
        std::fprintf(stderr,
                     "  FAIL: Crash or stale processes detected after run!\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

bool getFileState(const char* path, WIN32_FILE_ATTRIBUTE_DATA* data) {
    return GetFileAttributesExA(path, GetFileExInfoStandard, data) != 0;
}

bool fileExists(const char* path) {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

bool cleanFile(const std::string& path) {
    if (!fileExists(path.c_str())) return true;
    return DeleteFileA(path.c_str()) != 0;
}

// Print a failure banner and return the given code.
int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// ---------------------------------------------------------------------------
// Test state (shared between main and the worker thread)
// ---------------------------------------------------------------------------

struct TestState {
    int exitCode = 0;
};

// ---------------------------------------------------------------------------
// Main test logic (runs on a 64 MB-stack worker thread)
//
// TsDevLib is constructed AND destructed inside this thread so the
// thread-affine hidden EDIT control lives and dies on the same thread.
// ---------------------------------------------------------------------------
DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;

    // ================================================================
    // PHASE 1: Construct TsDevLib
    // ================================================================
    std::fprintf(stdout, "\n--- Phase 1: Construct TsDevLib ---\n");
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib();
    } catch (const std::exception& e) {
        ec = fail(e.what(), 10);
        return 0;
    }
    std::fprintf(stdout, "    TsDevLib constructed OK\n");

    // ================================================================
    // PHASE 2: Ensure test component exists (INSERT if needed)
    // ================================================================
    std::fprintf(stdout, "\n--- Phase 2: Ensure component exists ---\n");
    int progId = lib->findExistPos(kTestName, ComponentType::Strategy);
    std::fprintf(stdout, "    findExistPos(\"%s\") = %d\n", kTestName, progId);

    if (progId <= 0) {
        std::fprintf(stdout, "    Component not found — inserting...\n");

        // createEntry
        int errCode = 0;
        progId = lib->createEntry(ComponentType::Strategy, kTestName, errCode);
        std::fprintf(stdout, "    createEntry = %d, errCode = 0x%X\n",
                     progId, errCode);
        if (progId <= 0) {
            ec = fail("createEntry returned ProgId <= 0", 20);
            delete lib;
            return 0;
        }

        // saveSource
        bool saved = lib->saveSource(ComponentType::Strategy, progId,
                                     kTestSource);
        std::fprintf(stdout, "    saveSource = %s\n",
                     saved ? "true" : "false");
        if (!saved) {
            ec = fail("saveSource returned false", 30);
            delete lib;
            return 0;
        }

        // verifyModule (compile + persist .ell)
        std::string errors = lib->verifyModule(
            ComponentType::Strategy, progId);
        if (!errors.empty()) {
            std::fprintf(stderr, "    verifyModule errors:\n%s\n",
                         errors.c_str());
            ec = fail("verifyModule returned compilation errors", 40);
            delete lib;
            return 0;
        }
        std::fprintf(stdout, "    verifyModule: no errors (compilation OK)\n");

        // Re-check FindExistPos to confirm insertion persisted
        int recheck = lib->findExistPos(kTestName, ComponentType::Strategy);
        std::fprintf(stdout, "    post-insert findExistPos = %d\n", recheck);
        if (recheck <= 0) {
            ec = fail("component not found after insert", 45);
            delete lib;
            return 0;
        }
        progId = recheck;
    } else {
        std::fprintf(stdout, "    Component already exists (ProgId=%d)\n",
                     progId);
    }

    // ================================================================
    // PHASE 3: Capture pre-delete state
    // ================================================================
    std::fprintf(stdout, "\n--- Phase 3: Capture pre-delete state ---\n");

    // MWRKArea.wrk before
    WIN32_FILE_ATTRIBUTE_DATA wrkBefore;
    bool wrkBeforeOk = getFileState(kWrkPath, &wrkBefore);
    std::fprintf(stdout,
                 "    MWRKArea.wrk before: ok=%d size=%lu "
                 "mtime=0x%08X%08X\n",
                 (int)wrkBeforeOk,
                 wrkBeforeOk ? wrkBefore.nFileSizeLow : 0,
                 wrkBeforeOk ? wrkBefore.ftLastWriteTime.dwHighDateTime : 0,
                 wrkBeforeOk ? wrkBefore.ftLastWriteTime.dwLowDateTime : 0);

    // .ell file path and existence before delete
    std::string ellPath = lib->getEllPath(ComponentType::Strategy, progId);
    bool ellBefore = fileExists(ellPath.c_str());
    std::fprintf(stdout, "    .ell before: %s (%s)\n",
                 ellBefore ? "EXISTS" : "NOT FOUND", ellPath.c_str());

    // ================================================================
    // PHASE 4: Delete
    // ================================================================
    std::fprintf(stdout, "\n--- Phase 4: deleteEntry ---\n");
    std::fprintf(stdout, "    deleteEntry(Strategy, \"%s\")...\n", kTestName);
    bool deleted = lib->deleteEntry(ComponentType::Strategy, kTestName);
    std::fprintf(stdout, "    deleteEntry = %s\n",
                 deleted ? "true" : "false");
    if (!deleted) {
        ec = fail("deleteEntry returned false", 50);
        delete lib;
        return 0;
    }

    // ================================================================
    // PHASE 5: Destroy TsDevLib (OrlibExit persists the deletion)
    // ================================================================
    std::fprintf(stdout,
                 "\n--- Phase 5: Destroy TsDevLib (OrlibExit) ---\n");
    delete lib;
    lib = nullptr;
    std::fprintf(stdout, "    TsDevLib destroyed OK\n");

    // ================================================================
    // PHASE 6: Verify — MWRKArea.wrk timestamp or size changed
    // ================================================================
    std::fprintf(stdout,
                 "\n--- Phase 6: Verify MWRKArea.wrk changed ---\n");
    WIN32_FILE_ATTRIBUTE_DATA wrkAfter;
    bool wrkAfterOk = getFileState(kWrkPath, &wrkAfter);
    std::fprintf(stdout,
                 "    MWRKArea.wrk after: ok=%d size=%lu "
                 "mtime=0x%08X%08X\n",
                 (int)wrkAfterOk,
                 wrkAfterOk ? wrkAfter.nFileSizeLow : 0,
                 wrkAfterOk ? wrkAfter.ftLastWriteTime.dwHighDateTime : 0,
                 wrkAfterOk ? wrkAfter.ftLastWriteTime.dwLowDateTime : 0);

    bool wrkChanged = false;
    if (wrkBeforeOk && wrkAfterOk) {
        if (wrkAfter.ftLastWriteTime.dwLowDateTime
                != wrkBefore.ftLastWriteTime.dwLowDateTime ||
            wrkAfter.ftLastWriteTime.dwHighDateTime
                != wrkBefore.ftLastWriteTime.dwHighDateTime ||
            wrkAfter.nFileSizeLow != wrkBefore.nFileSizeLow) {
            wrkChanged = true;
        }
    }
    if (wrkChanged) {
        std::fprintf(stdout,
                     "    PASS: MWRKArea.wrk timestamp or size changed\n");
    } else {
        std::fprintf(stdout, "    FAIL: MWRKArea.wrk unchanged\n");
        if (ec == 0) ec = fail("MWRKArea.wrk did not change", 60);
    }

    // ================================================================
    // PHASE 7: Verify — Fresh FindExistPos returns <= 0
    // ================================================================
    std::fprintf(stdout, "\n--- Phase 7: Fresh FindExistPos ---\n");
    try {
        lib = new TsDevLib();
    } catch (const std::exception& e) {
        if (ec == 0) ec = fail(e.what(), 70);
        return 0;
    }
    std::fprintf(stdout, "    Fresh TsDevLib constructed OK\n");

    int finalCheck = lib->findExistPos(kTestName, ComponentType::Strategy);
    std::fprintf(stdout, "    fresh findExistPos = %d\n", finalCheck);
    if (finalCheck <= 0) {
        std::fprintf(stdout,
                     "    PASS: Entry gone (findExistPos <= 0)\n");
    } else {
        std::fprintf(stderr,
                     "    FAIL: Entry still exists (ProgId=%d)\n",
                     finalCheck);
        if (ec == 0) ec = fail("entry still exists after delete", 80);
        // Best-effort cleanup of the stale .ell
        std::string p =
            lib->getEllPath(ComponentType::Strategy, finalCheck);
        cleanFile(p);
    }

    // ================================================================
    // PHASE 8: Verify — No .ell file remains on disk (and check .xml)
    // ================================================================
    std::fprintf(stdout, "\n--- Phase 8: Verify .ell file gone ---\n");
    bool ellAfter = fileExists(ellPath.c_str());
    if (!ellAfter) {
        std::fprintf(stdout, "    PASS: .ell file not found (%s)\n",
                     ellPath.c_str());
    } else {
        std::fprintf(stderr, "    FAIL: .ell file still exists (%s)\n",
                     ellPath.c_str());
        cleanFile(ellPath);
        if (ec == 0) ec = fail(".ell file not deleted from disk", 90);
    }

    // Also check the .xml (debug symbols) file. Standalone mode does not
    // always produce .xml, so "not found" is PASS and "found" is a
    // non-fatal warning (does not fail the test).
    std::string xmlPath = lib->getXmlPath(ComponentType::Strategy, progId);
    bool xmlAfter = fileExists(xmlPath.c_str());
    if (!xmlAfter) {
        std::fprintf(stdout, "    PASS: .xml file not found (%s)\n",
                     xmlPath.c_str());
    } else {
        std::fprintf(stdout,
                     "    WARNING: .xml file still exists (%s) — "
                     "non-fatal (standalone mode may not produce .xml)\n",
                     xmlPath.c_str());
        cleanFile(xmlPath);
    }

    // Destroy fresh TsDevLib
    delete lib;
    lib = nullptr;
    std::fprintf(stdout, "    Fresh TsDevLib destroyed OK\n");

    // ================================================================
    // SUMMARY
    // ================================================================
    if (ec == 0) {
        std::fprintf(stdout,
                     "\n=== ALL CHECKS PASSED ===\n"
                     "  - Component existed before delete\n"
                     "  - deleteEntry returned true\n"
                     "  - MWRKArea.wrk timestamp/size changed\n"
                     "  - Fresh FindExistPos <= 0 (entry gone)\n"
                     "  - No .ell file remains on disk\n"
                     "  - .xml file checked (non-fatal if present)\n");
    } else {
        std::fprintf(stderr,
                     "\n=== CHECKS FAILED (exit code %d) ===\n", ec);
    }

    return 0;
}

// ---------------------------------------------------------------------------
// SEH wrapper — POD-only (no C++ objects with destructors) so __try is
// legal. Catches any unhandled structured exception from runTest (e.g.,
// access violation in the delete orchestrator, InitGlobalLookup, or
// OrlibExit) and converts it to a nonzero exit code instead of letting
// the process crash with an indeterminate exit code.
// ---------------------------------------------------------------------------
DWORD WINAPI threadProc(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    __try {
        runTest(param);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr,
                     "\n[FATAL] Unhandled SEH exception 0x%08X\n",
                     GetExceptionCode());
        st->exitCode = 99;
    }
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_delete_diag: DELETE flow diagnostic ===\n");
    std::fprintf(stdout, "Strategy name: %s\n", kTestName);

    // --- Pre-run: check for stale/conflicting processes (report only) ---
    preRunProcessCheck();

    // --- Run the test on a 64 MB-stack worker thread ---
    // The EasyLanguage compiler (invoked via the delete orchestrator) needs
    // far more than the default 1 MB stack, and the hidden EDIT control is
    // thread-affine so TsDevLib must live on this thread.
    TestState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  threadProc, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    // --- Post-run: check for WerFault/crash processes ---
    bool noCrashes = postRunCrashCheck();

    // --- Determine final exit code ---
    int exitCode = st.exitCode;
    if (!noCrashes && exitCode == 0) {
        // Crashes detected even though the test logic thought it passed.
        exitCode = 98;
    }

    std::fprintf(stdout,
                 "\n=== test_delete_diag complete (exit %d) ===\n",
                 exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
