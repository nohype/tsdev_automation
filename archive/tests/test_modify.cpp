// test_modify.cpp
//
// Integration test for the implemented MODIFY flow through the tsdev_lib
// wrapper. `modifyEntry` is declared by TsDevLib and implemented in
// src/tsdev_lib.cpp; this test exercises that existing library operation.
//
// MODIFY requirements verified by this test:
//   1. Find the existing component by name+type (FindExistPos).
//   2. Return Result{success=false} if the component does not exist.
//   3. Replace the component's EasyLanguage source with newSource (saveSource
//      on the EXISTING ProgId — NO createEntry).
//   4. Recompile and re-verify (verifyModule).
//   5. Fail loudly on compilation errors — return the error string in
//      Result.message so the caller can report it.
//   6. Persist the new source (the .ell file should be updated).
//   7. The component's ProgId should NOT change (modification, not re-creation).
//
// Test flow:
//   SETUP:   Ensure test component (zzz_cli_modify_test, Strategy) exists with
//            source A (Buy order). Insert if needed.
//   RECORD:  Record .ell file path + timestamp/size.
//   MODIFY:  modifyEntry(Strategy, name, sourceB) — changes Buy -> Sell.
//   VERIFY:  - modifyEntry returned success (no compile errors).
//            - Component still exists; findExistPos returns the SAME ProgId.
//            - .ell file exists and its timestamp changed (was updated).
//            - verifyModule on a fresh check returns empty string.
//   NEG:     modifyEntry(Strategy, "zzz_nonexistent_xyz", sourceB) must
//            return Result{success=false}.
//   ERR:     modifyEntry(Strategy, name, invalidSource) must return
//            Result{success=false} with a non-empty error string.
//   CLEANUP: deleteEntry so the test is repeatable.
//
// Runs on a 64 MB-stack worker thread (the EasyLanguage compiler and the
// thread-affine hidden EDIT control require it), matching test_insert.cpp and
// test_delete_diag.cpp. An SEH wrapper around the worker thread body catches
// any unhandled structured exception and converts it to a nonzero exit code.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

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

const char* kTestName = "zzz_cli_modify_test";

// Source A (initial): a simple market BUY order.
const char* kSourceA =
    "{ zzz_cli_modify_test }\r\n"
    "Buy (\"EnterLong\") next bar at Market;\r\n";

// Source B (modified): changes the strategy from Buy to Sell.
const char* kSourceB =
    "{ zzz_cli_modify_test }\r\n"
    "Sell (\"EnterShort\") next bar at Market;\r\n";

// Invalid source: a syntax error. Must produce a compile error.
const char* kInvalidSource =
    "{ zzz_cli_modify_test }\r\n"
    "ThisIsNotValidEasyLanguage syntax!!!;\r\n";

const char* kNonexistentName = "zzz_nonexistent_xyz";

// ---------------------------------------------------------------------------
// Process scanning (report only — do NOT kill)
// ---------------------------------------------------------------------------

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

bool postRunCrashCheck() {
    const char* names[] = {
        "WerFault.exe",
        "test_modify.exe"
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
// Ensure the test component exists with source A.
// Returns the ProgId (>0) on success, 0 on failure (exitCode set).
// ---------------------------------------------------------------------------
int ensureComponentExists(TsDevLib* lib, int& ec) {
    int progId = lib->findExistPos(kTestName, ComponentType::Strategy);
    std::fprintf(stdout, "    findExistPos(\"%s\") = %d\n", kTestName, progId);

    if (progId <= 0) {
        std::fprintf(stdout, "    Component not found — inserting...\n");
        int errCode = 0;
        progId = lib->createEntry(ComponentType::Strategy, kTestName, errCode);
        std::fprintf(stdout, "    createEntry = %d, errCode = 0x%X\n",
                     progId, errCode);
        if (progId <= 0) {
            ec = fail("createEntry returned ProgId <= 0", 20);
            return 0;
        }
    } else {
        std::fprintf(stdout, "    Component already exists (ProgId=%d)\n",
                     progId);
    }

    bool saved = lib->saveSource(ComponentType::Strategy, progId, kSourceA);
    std::fprintf(stdout, "    saveSource(A) = %s\n", saved ? "true" : "false");
    if (!saved) {
        ec = fail("saveSource(A) returned false", 30);
        return 0;
    }

    std::string errors = lib->verifyModule(ComponentType::Strategy, progId);
    if (!errors.empty()) {
        std::fprintf(stderr, "    verifyModule(A) errors:\n%s\n",
                     errors.c_str());
        ec = fail("verifyModule(A) returned compilation errors", 40);
        return 0;
    }
    std::fprintf(stdout, "    verifyModule(A): no errors (compilation OK)\n");
    return progId;
}

// ---------------------------------------------------------------------------
// Main test logic (runs on a 64 MB-stack worker thread)
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
    // PHASE 2: Ensure test component exists with source A
    // ================================================================
    std::fprintf(stdout, "\n--- Phase 2: Ensure component exists (source A) ---\n");
    int progId = ensureComponentExists(lib, ec);
    if (progId <= 0) {
        delete lib;
        return 0;
    }
    std::fprintf(stdout, "    Component ready: ProgId=%d\n", progId);

    // ================================================================
    // PHASE 3: Record .ell file state (path + timestamp/size)
    // ================================================================
    std::fprintf(stdout, "\n--- Phase 3: Record .ell state ---\n");
    std::string ellPath = lib->getEllPath(ComponentType::Strategy, progId);
    WIN32_FILE_ATTRIBUTE_DATA ellBefore;
    bool ellBeforeOk = getFileState(ellPath.c_str(), &ellBefore);
    std::fprintf(stdout,
                 "    .ell before: ok=%d size=%lu mtime=0x%08X%08X\n"
                 "    path: %s\n",
                 (int)ellBeforeOk,
                 ellBeforeOk ? ellBefore.nFileSizeLow : 0,
                 ellBeforeOk ? ellBefore.ftLastWriteTime.dwHighDateTime : 0,
                 ellBeforeOk ? ellBefore.ftLastWriteTime.dwLowDateTime : 0,
                 ellPath.c_str());
    if (!ellBeforeOk) {
        ec = fail(".ell file does not exist before modify", 50);
        delete lib;
        return 0;
    }

    // ================================================================
    // PHASE 4: Modify — change Buy -> Sell (source B)
    // ================================================================
    std::fprintf(stdout, "\n--- Phase 4: modifyEntry(Strategy, name, sourceB) ---\n");
    Result modRes = lib->modifyEntry(ComponentType::Strategy, kTestName,
                                     kSourceB);
    std::fprintf(stdout, "    modifyEntry: success=%d message=\"%s\"\n",
                 (int)modRes.success, modRes.message.c_str());
    if (!modRes.success) {
        ec = fail("modifyEntry returned failure on valid source B", 60);
        delete lib;
        return 0;
    }

    // ================================================================
    // PHASE 5: Verify
    // ================================================================
    std::fprintf(stdout, "\n--- Phase 5: Verify ---\n");

    // 5a. Component still exists with the SAME ProgId.
    int progIdAfter = lib->findExistPos(kTestName, ComponentType::Strategy);
    std::fprintf(stdout, "    findExistPos after modify = %d (was %d)\n",
                 progIdAfter, progId);
    if (progIdAfter <= 0) {
        ec = fail("component not found after modify", 70);
        delete lib;
        return 0;
    }
    if (progIdAfter != progId) {
        std::fprintf(stderr,
                     "    FAIL: ProgId changed %d -> %d (must NOT change)\n",
                     progId, progIdAfter);
        ec = fail("ProgId changed after modify (must stay the same)", 75);
        delete lib;
        return 0;
    }
    std::fprintf(stdout, "    PASS: ProgId unchanged (%d)\n", progId);

    // 5b. .ell file exists and timestamp/size changed.
    WIN32_FILE_ATTRIBUTE_DATA ellAfter;
    bool ellAfterOk = getFileState(ellPath.c_str(), &ellAfter);
    std::fprintf(stdout,
                 "    .ell after: ok=%d size=%lu mtime=0x%08X%08X\n",
                 (int)ellAfterOk,
                 ellAfterOk ? ellAfter.nFileSizeLow : 0,
                 ellAfterOk ? ellAfter.ftLastWriteTime.dwHighDateTime : 0,
                 ellAfterOk ? ellAfter.ftLastWriteTime.dwLowDateTime : 0);
    if (!ellAfterOk) {
        ec = fail(".ell file missing after modify", 80);
        delete lib;
        return 0;
    }
    bool ellChanged = false;
    if (ellAfter.ftLastWriteTime.dwLowDateTime
            != ellBefore.ftLastWriteTime.dwLowDateTime ||
        ellAfter.ftLastWriteTime.dwHighDateTime
            != ellBefore.ftLastWriteTime.dwHighDateTime ||
        ellAfter.nFileSizeLow != ellBefore.nFileSizeLow) {
        ellChanged = true;
    }
    if (ellChanged) {
        std::fprintf(stdout, "    PASS: .ell timestamp/size changed\n");
    } else {
        ec = fail(".ell file not updated after modify", 85);
        delete lib;
        return 0;
    }

    // 5c. Fresh verifyModule returns empty string (no compile errors).
    std::string freshErrors =
        lib->verifyModule(ComponentType::Strategy, progId);
    if (freshErrors.empty()) {
        std::fprintf(stdout, "    PASS: fresh verifyModule has no errors\n");
    } else {
        std::fprintf(stderr, "    FAIL: fresh verifyModule errors:\n%s\n",
                     freshErrors.c_str());
        ec = fail("fresh verifyModule returned compilation errors", 90);
        delete lib;
        return 0;
    }

    // ================================================================
    // PHASE 6: Negative test — modify a nonexistent component
    // ================================================================
    std::fprintf(stdout, "\n--- Phase 6: Negative test (nonexistent) ---\n");
    Result negRes = lib->modifyEntry(ComponentType::Strategy,
                                     kNonexistentName, kSourceB);
    std::fprintf(stdout, "    modifyEntry(nonexistent): success=%d\n",
                 (int)negRes.success);
    if (negRes.success) {
        ec = fail("modifyEntry succeeded on a nonexistent component", 100);
        delete lib;
        return 0;
    }
    std::fprintf(stdout, "    PASS: modifyEntry(nonexistent) returned failure\n");

    // ================================================================
    // PHASE 7: Compile-error test — invalid source
    // ================================================================
    std::fprintf(stdout, "\n--- Phase 7: Compile-error test (invalid source) ---\n");
    Result errRes = lib->modifyEntry(ComponentType::Strategy, kTestName,
                                     kInvalidSource);
    std::fprintf(stdout,
                 "    modifyEntry(invalid): success=%d message=\"%s\"\n",
                 (int)errRes.success, errRes.message.c_str());
    if (errRes.success) {
        ec = fail("modifyEntry succeeded on invalid source", 110);
        delete lib;
        return 0;
    }
    if (errRes.message.empty()) {
        ec = fail("modifyEntry failed but returned empty error string", 115);
        delete lib;
        return 0;
    }
    // TVS-A.4: The error message must contain REAL compiler diagnostics with
    // readable text — not the synthesized fallback AND not garbage bytes from
    // a misinterpreted std::string object (ABI bug). Real compiler error text
    // contains alphabetic characters (e.g. "Expected", "error", "syntax").
    if (errRes.message.find("VerifyModule_New returned") != std::string::npos) {
        ec = fail("modifyEntry returned synthesized fallback instead of real compiler errors", 116);
        delete lib;
        return 0;
    }
    bool hasAlpha = false;
    for (char c : errRes.message) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            hasAlpha = true;
            break;
        }
    }
    if (!hasAlpha) {
        ec = fail("modifyEntry error message has no alphabetic characters (garbage/empty, not real compiler text)", 117);
        delete lib;
        return 0;
    }
    if (errRes.message.find("Line") == std::string::npos) {
        ec = fail("modifyEntry error message lacks source location (expected 'Line N' prefix)", 118);
        delete lib;
        return 0;
    }
    std::fprintf(stdout,
                 "    PASS: error message includes source location info\n");
    std::fprintf(stdout,
                 "    PASS: error message is real compiler text (not synthesized fallback, not garbage)\n");
    std::fprintf(stdout,
                 "    PASS: modifyEntry(invalid) returned failure with error\n");

    // After the compile-error test, re-save the valid source B so the
    // component is left in a valid state before cleanup.
    std::fprintf(stdout, "    Restoring valid source B before cleanup...\n");
    Result restoreRes = lib->modifyEntry(ComponentType::Strategy, kTestName,
                                         kSourceB);
    if (!restoreRes.success) {
        std::fprintf(stderr,
                     "    WARNING: could not restore source B after error test\n");
    }

    // ================================================================
    // PHASE 8: Cleanup — delete the test component
    // ================================================================
    std::fprintf(stdout, "\n--- Phase 8: Cleanup (deleteEntry) ---\n");
    bool deleted = lib->deleteEntry(ComponentType::Strategy, kTestName);
    std::fprintf(stdout, "    deleteEntry = %s\n", deleted ? "true" : "false");
    if (!deleted) {
        ec = fail("deleteEntry returned false during cleanup", 120);
        delete lib;
        return 0;
    }

    delete lib;
    lib = nullptr;
    std::fprintf(stdout, "    TsDevLib destroyed OK\n");

    // ================================================================
    // SUMMARY
    // ================================================================
    if (ec == 0) {
        std::fprintf(stdout,
                     "\n=== ALL CHECKS PASSED ===\n"
                     "  - Component existed with source A\n"
                     "  - modifyEntry(sourceB) succeeded\n"
                     "  - ProgId unchanged after modify\n"
                     "  - .ell file updated (timestamp/size changed)\n"
                     "  - Fresh verifyModule: no errors\n"
                     "  - Negative test: nonexistent -> failure\n"
                     "  - Compile-error test: invalid source -> failure+error\n"
                     "  - Cleanup: component deleted\n");
    } else {
        std::fprintf(stderr,
                     "\n=== CHECKS FAILED (exit code %d) ===\n", ec);
    }

    return 0;
}

// ---------------------------------------------------------------------------
// SEH wrapper — POD-only (no C++ objects with destructors) so __try is legal.
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
                 "=== test_modify: MODIFY flow end-to-end ===\n");
    std::fprintf(stdout, "Strategy name: %s\n", kTestName);

    // --- Run the test on a 64 MB-stack worker thread ---
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

    int exitCode = st.exitCode;
    if (!noCrashes && exitCode == 0) {
        exitCode = 98;
    }

    std::fprintf(stdout,
                 "\n=== test_modify complete (exit %d) ===\n", exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
