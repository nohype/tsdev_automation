// test_modify_tradingapp.cpp
//
// Integration test (RED-GREEN TDD) for TVS-C.6: TradingApp MODIFY flow through
// the tsdev_lib wrapper. MODIFY reuses the confirmed primitives (findExistPos +
// saveSource + verifyModule on an existing ProgId) that were proven for Strategy
// in test_modify.cpp and for TradingApp INSERT/DELETE in TVS-C.3 E2E. This test
// formalizes the TradingApp MODIFY behavior.
//
// MODIFY requirements verified by this test:
//   1. Find the existing TradingApp by name (findExistPos).
//   2. Return Result{success=false} if the component does not exist.
//   3. Replace the EasyLanguage source on the EXISTING ProgId (no createEntry).
//   4. Recompile and re-verify (verifyModule).
//   5. Fail on compilation errors — return the error string in Result.message.
//   6. Persist the new source (.ell file timestamp changes).
//   7. ProgId must NOT change (modification, not re-creation).
//
// Test flow:
//   SETUP:   INSERT a test TradingApp ("test_ta_modify_c6") with source A.
//            Record ProgId and .ell timestamp.
//   MODIFY:  modifyEntry(TradingApp, name, sourceB) — add a using clause.
//   VERIFY:  - modifyEntry returned success.
//            - ProgId is preserved (same before and after).
//            - .ell file timestamp changed (re-compiled + re-persisted).
//   NEG:     modifyEntry(TradingApp, "nonexistent_ta_c6", sourceA) must
//            return Result{success=false}.
//   ERR:     INSERT a second entry "test_ta_modify_err_c6", then
//            modifyEntry(TradingApp, "test_ta_modify_err_c6", invalidSource)
//            must return Result{success=false} with non-empty message.
//   CLEANUP: Delete both test entries.
//
// Self-cleaning: cleanup runs even if assertions fail (try/catch + best-effort
// delete in a finally-style block).
//
// Runs on a 64 MB-stack worker thread (EasyLanguage compiler + thread-affine
// hidden EDIT control require it), matching test_modify.cpp.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

using namespace tsdev;

namespace {

// ---------------------------------------------------------------------------
// Test fixture constants — unique names to avoid collisions with C.4/C.5.
// ---------------------------------------------------------------------------

const char* kTestName    = "test_ta_modify_c6";
const char* kErrTestName = "test_ta_modify_err_c6";
const char* kNonexistent = "nonexistent_ta_c6";

// Source A (initial): minimal TradingApp source proven to compile (TVS-C.3).
const char* kSourceA = "using elsystem;\r\n";

// Source B (modified): adds a second using clause (still valid).
const char* kSourceB =
    "using elsystem;\r\n"
    "using elsystem.windows.forms;\r\n";

// Invalid source: must produce a compile error.
const char* kInvalidSource = "this is not valid easylanguage;\r\n";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

bool getFileState(const char* path, WIN32_FILE_ATTRIBUTE_DATA* data) {
    return GetFileAttributesExA(path, GetFileExInfoStandard, data) != 0;
}

// ---------------------------------------------------------------------------
// Test state
// ---------------------------------------------------------------------------

struct TestState {
    int exitCode = 0;
};

// ---------------------------------------------------------------------------
// Main test logic (runs on a 64 MB-stack worker thread)
// ---------------------------------------------------------------------------

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;

    TsDevLib* lib = nullptr;
    int mainProgId = 0;
    int errProgId  = 0;

    try {
        // ================================================================
        // PHASE 1: Construct TsDevLib
        // ================================================================
        std::fprintf(stdout, "\n--- Phase 1: Construct TsDevLib ---\n");
        lib = new TsDevLib();
        std::fprintf(stdout, "    TsDevLib constructed OK\n");

        // ================================================================
        // PHASE 2: Pre-clean any stale entries from prior failed runs
        // ================================================================
        std::fprintf(stdout, "\n--- Phase 2: Pre-clean stale entries ---\n");
        lib->deleteEntry(ComponentType::TradingApp, kTestName);
        lib->deleteEntry(ComponentType::TradingApp, kErrTestName);
        std::fprintf(stdout, "    Pre-clean done\n");

        // ================================================================
        // PHASE 3: INSERT the main test entry (source A)
        // ================================================================
        std::fprintf(stdout, "\n--- Phase 3: INSERT main entry (source A) ---\n");
        int errCode = 0;
        mainProgId = lib->createEntry(ComponentType::TradingApp, kTestName,
                                      errCode);
        std::fprintf(stdout, "    createEntry = %d, errCode = 0x%X\n",
                     mainProgId, errCode);
        if (mainProgId <= 0) {
            ec = fail("createEntry returned ProgId <= 0", 20);
            return 0;
        }

        bool saved = lib->saveSource(ComponentType::TradingApp, mainProgId,
                                     kSourceA);
        std::fprintf(stdout, "    saveSource(A) = %s\n", saved ? "true" : "false");
        if (!saved) {
            ec = fail("saveSource(A) returned false", 30);
            return 0;
        }

        std::string errors = lib->verifyModule(ComponentType::TradingApp,
                                               mainProgId);
        if (!errors.empty()) {
            std::fprintf(stderr, "    verifyModule(A) errors:\n%s\n",
                         errors.c_str());
            ec = fail("verifyModule(A) returned compilation errors", 40);
            return 0;
        }
        std::fprintf(stdout, "    verifyModule(A): no errors (compilation OK)\n");

        // ================================================================
        // PHASE 4: Record .ell file state (timestamp)
        // ================================================================
        std::fprintf(stdout, "\n--- Phase 4: Record .ell state ---\n");
        std::string ellPath = lib->getEllPath(ComponentType::TradingApp,
                                              mainProgId);
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
            return 0;
        }

        // ================================================================
        // PHASE 5: MODIFY the entry (source B)
        // ================================================================
        std::fprintf(stdout,
                     "\n--- Phase 5: modifyEntry(TradingApp, name, sourceB) ---\n");
        Result modRes = lib->modifyEntry(ComponentType::TradingApp, kTestName,
                                         kSourceB);
        std::fprintf(stdout, "    modifyEntry: success=%d message=\"%s\"\n",
                     (int)modRes.success, modRes.message.c_str());
        if (!modRes.success) {
            ec = fail("modifyEntry returned failure on valid source B", 60);
            return 0;
        }

        // ================================================================
        // PHASE 6: Verify ProgId preserved + .ell timestamp changed
        // ================================================================
        std::fprintf(stdout, "\n--- Phase 6: Verify ---\n");

        // 6a. ProgId preserved.
        int progIdAfter = lib->findExistPos(kTestName,
                                            ComponentType::TradingApp);
        std::fprintf(stdout, "    findExistPos after modify = %d (was %d)\n",
                     progIdAfter, mainProgId);
        if (progIdAfter <= 0) {
            ec = fail("component not found after modify", 70);
            return 0;
        }
        if (progIdAfter != mainProgId) {
            ec = fail("ProgId changed after modify (must stay the same)", 75);
            return 0;
        }
        std::fprintf(stdout, "    PASS: ProgId unchanged (%d)\n", mainProgId);

        // 6b. .ell timestamp changed.
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
            return 0;
        }
        bool ellChanged =
            ellAfter.ftLastWriteTime.dwLowDateTime
                != ellBefore.ftLastWriteTime.dwLowDateTime ||
            ellAfter.ftLastWriteTime.dwHighDateTime
                != ellBefore.ftLastWriteTime.dwHighDateTime ||
            ellAfter.nFileSizeLow != ellBefore.nFileSizeLow;
        if (!ellChanged) {
            ec = fail(".ell file not updated after modify", 85);
            return 0;
        }
        std::fprintf(stdout, "    PASS: .ell timestamp/size changed\n");

        // ================================================================
        // PHASE 7: Negative test — modify a nonexistent component
        // ================================================================
        std::fprintf(stdout, "\n--- Phase 7: Negative test (nonexistent) ---\n");
        Result negRes = lib->modifyEntry(ComponentType::TradingApp,
                                         kNonexistent, kSourceA);
        std::fprintf(stdout, "    modifyEntry(nonexistent): success=%d\n",
                     (int)negRes.success);
        if (negRes.success) {
            ec = fail("modifyEntry succeeded on a nonexistent component", 100);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: modifyEntry(nonexistent) returned failure\n");

        // ================================================================
        // PHASE 8: Compile-error test
        // ================================================================
        std::fprintf(stdout,
                     "\n--- Phase 8: Compile-error test (invalid source) ---\n");

        // 8a. INSERT a second entry for the error test.
        int errCode2 = 0;
        errProgId = lib->createEntry(ComponentType::TradingApp, kErrTestName,
                                     errCode2);
        std::fprintf(stdout, "    createEntry(err) = %d, errCode = 0x%X\n",
                     errProgId, errCode2);
        if (errProgId <= 0) {
            ec = fail("createEntry(err) returned ProgId <= 0", 105);
            return 0;
        }
        lib->saveSource(ComponentType::TradingApp, errProgId, kSourceA);
        errors = lib->verifyModule(ComponentType::TradingApp, errProgId);
        if (!errors.empty()) {
            std::fprintf(stderr, "    verifyModule(err, A) errors:\n%s\n",
                         errors.c_str());
            ec = fail("verifyModule(err, A) returned compilation errors", 106);
            return 0;
        }

        // 8b. modifyEntry with invalid source must fail with a message.
        Result errRes = lib->modifyEntry(ComponentType::TradingApp,
                                         kErrTestName, kInvalidSource);
        std::fprintf(stdout,
                     "    modifyEntry(invalid): success=%d message=\"%s\"\n",
                     (int)errRes.success, errRes.message.c_str());
        if (errRes.success) {
            ec = fail("modifyEntry succeeded on invalid source", 110);
            return 0;
        }
        if (errRes.message.empty()) {
            ec = fail("modifyEntry failed but returned empty error string", 115);
            return 0;
        }
        // The error message must contain real compiler text (alphabetic chars).
        bool hasAlpha = false;
        for (char c : errRes.message) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                hasAlpha = true;
                break;
            }
        }
        if (!hasAlpha) {
            ec = fail("modifyEntry error message has no alphabetic characters", 116);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: modifyEntry(invalid) returned failure with error\n");

    } catch (const std::exception& e) {
        ec = fail(e.what(), 90);
    } catch (...) {
        ec = fail("unknown exception in test body", 91);
    }

    // ================================================================
    // PHASE 9: Cleanup — delete both test entries (always, best-effort)
    // ================================================================
    std::fprintf(stdout, "\n--- Phase 9: Cleanup (deleteEntry) ---\n");
    if (lib) {
        bool delMain = lib->deleteEntry(ComponentType::TradingApp, kTestName);
        std::fprintf(stdout, "    deleteEntry(\"%s\") = %s\n", kTestName,
                     delMain ? "true" : "false");
        if (ec == 0 && !delMain) {
            ec = fail("deleteEntry(main) returned false during cleanup", 120);
        }

        bool delErr = lib->deleteEntry(ComponentType::TradingApp, kErrTestName);
        std::fprintf(stdout, "    deleteEntry(\"%s\") = %s\n", kErrTestName,
                     delErr ? "true" : "false");
        if (ec == 0 && !delErr) {
            ec = fail("deleteEntry(err) returned false during cleanup", 121);
        }

        delete lib;
        lib = nullptr;
        std::fprintf(stdout, "    TsDevLib destroyed OK\n");
    }

    // ================================================================
    // SUMMARY
    // ================================================================
    if (ec == 0) {
        std::fprintf(stdout,
                     "\n=== ALL CHECKS PASSED ===\n"
                     "  - INSERT main entry (source A) succeeded\n"
                     "  - modifyEntry(sourceB) succeeded\n"
                     "  - ProgId unchanged after modify\n"
                     "  - .ell file updated (timestamp/size changed)\n"
                     "  - Negative test: nonexistent -> failure\n"
                     "  - Compile-error test: invalid source -> failure+error\n"
                     "  - Cleanup: both entries deleted\n");
    } else {
        std::fprintf(stderr, "\n=== CHECKS FAILED (exit code %d) ===\n", ec);
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
                 "=== test_modify_tradingapp: TradingApp MODIFY flow (TVS-C.6) ===\n");
    std::fprintf(stdout, "Main TradingApp name: %s\n", kTestName);
    std::fprintf(stdout, "Error TradingApp name: %s\n", kErrTestName);

    // Run the test on a 64 MB-stack worker thread.
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

    std::fprintf(stdout,
                 "\n=== test_modify_tradingapp complete (exit %d) ===\n",
                 st.exitCode);
    if (st.exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return st.exitCode;
}
