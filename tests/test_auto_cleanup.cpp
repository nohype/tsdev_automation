// test_auto_cleanup.cpp
//
// Integration test (GREEN) for the TVS-L.10 create-path cleanup architecture.
//
// History: this test was originally written for TD-001, when the partial-entry
// cleanup on compile failure lived INSIDE verifyModule (keyed off the
// process-global g_lastCreated). TVS-L.10 moved that cleanup OUT of
// verifyModule and into doInsert (the SINGLE create-path cleanup owner in the
// CLI). verifyModule now REPORTS the failure stage via an out-parameter but
// does NOT clean up — the caller owns cleanup.
//
// This test verifies the NEW TVS-L.10 contract at the library layer:
//   1. createEntry + saveSource + verifyModule(&outStage) with invalid source.
//   2. verifyModule returns a NON-EMPTY error string (compilation FAILED).
//   3. verifyModule sets *outStage = VerifyModule_CompileErrors (the stage is
//      reported so the caller / doInsert can route through
//      requiresCreatePathCleanup() and record WHICH path failed).
//   4. The partial entry STILL EXISTS after verifyModule returns (verifyModule
//      no longer auto-cleans up — that is doInsert's job). findExistPos > 0.
//   5. An explicit deleteEntry (simulating what doInsert does on a
//      post-createEntry failure) removes the partial entry + both artifacts.
//      findExistPos then returns <= 0.
//
// The test is self-cleaning: if any assertion fails, it deletes the entry via
// deleteEntry so the test is repeatable.
//
// Exit code: 0 on PASS, nonzero per failing step.
//
// NOTE: The EasyLanguage compiler (invoked via VerifyModule_New) requires a
// large stack, AND the hidden EDIT control created by TsDevLib is a Win32
// window (thread-affine). The reference probe (archive/elevated_test20.cpp) runs its
// ENTIRE flow — including orlib init and window creation — in a worker thread
// with a 64 MB stack. We do the same: TsDevLib is constructed AND destructed
// inside the worker thread.

#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace tsdev;

namespace {

// Test fixture constants.
// A unique name minimizes collision risk with other tests. The "zzz_" prefix
// sorts it to the end of the warehouse and signals a test entry.
const char* kTestName = "zzz_test_autocleanup";

// Invalid EasyLanguage source — this will FAIL compilation. The body contains
// a syntax error (`var x = ;` — missing RHS expression) that the EasyLanguage
// compiler will reject. The brace text is arbitrary fixture source content only;
// it is not a metadata convention or lookup mechanism. The item name is passed
// through the API, and stored names/types/ProgIds come from orlib20 warehouse
// records/APIs; the body is deliberately broken.
const char* kInvalidSource =
    "{ zzz_test_autocleanup }\r\n"
    "using elsystem;\r\n"
    "var x = ;\r\n";

// Print a failure banner and return the given code.
int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s\n", msg);
    return code;
}

// Shared state between main and the worker thread.
struct TestState {
    int exitCode = 0;
};

// The full test flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);

    // --- Step 1: Construct TsDevLib ---
    std::fprintf(stdout, "\n[1] Constructing TsDevLib...\n");
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib();
    } catch (const std::exception& e) {
        st->exitCode = fail(e.what(), 10);
        return 0;
    }
    std::fprintf(stdout, "    TsDevLib constructed OK\n");

    // --- Step 2: Pre-clean — ensure the entry does not already exist ---
    std::fprintf(stdout, "\n[2] Pre-clean: findExistPos(\"%s\", Strategy)...\n",
                 kTestName);
    int existing = lib->findExistPos(kTestName, ComponentType::Strategy);
    std::fprintf(stdout, "    findExistPos = %d\n", existing);
    if (existing > 0) {
        std::fprintf(stdout,
                     "    Entry already exists (ProgId=%d) -- deleting...\n",
                     existing);
        bool deleted = lib->deleteEntry(ComponentType::Strategy, kTestName);
        std::fprintf(stdout, "    deleteEntry = %s\n",
                     deleted ? "true" : "false");
    }

    // --- Step 3: createEntry ---
    std::fprintf(stdout, "\n[3] createEntry(Strategy, \"%s\")...\n",
                 kTestName);
    int errCode = 0;
    int progId = lib->createEntry(ComponentType::Strategy, kTestName, errCode);
    std::fprintf(stdout, "    createEntry = %d, errCode = 0x%X\n",
                 progId, errCode);
    if (progId <= 0) {
        st->exitCode = fail("createEntry returned ProgId <= 0", 20);
        delete lib;
        return 0;
    }

    // --- Step 4: saveSource (invalid source) ---
    std::fprintf(stdout, "\n[4] saveSource(Strategy, %d, invalidSource)...\n",
                 progId);
    bool saved = lib->saveSource(ComponentType::Strategy, progId,
                                 kInvalidSource);
    std::fprintf(stdout, "    saveSource = %s\n", saved ? "true" : "false");
    if (!saved) {
        st->exitCode = fail("saveSource returned false", 30);
        // Clean up the partial entry before exiting.
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }

    // --- Step 5: verifyModule — expect compilation FAILURE + stage report ---
    // TVS-L.10: verifyModule now takes an optional CreatePathFailureStage*
    // out-param. On a non-empty (failure) return it MUST set *outStage to the
    // matching enumerator. For a compile error that is VerifyModule_CompileErrors.
    // verifyModule NO LONGER auto-cleans up the partial entry — that is the
    // caller's (doInsert's) job.
    std::fprintf(stdout, "\n[5] verifyModule(Strategy, %d, &outStage)...\n",
                 progId);
    tsdev::CreatePathFailureStage stage =
        tsdev::CreatePathFailureStage::VerifyModule_CompileErrors; // sentinel
    std::string errors =
        lib->verifyModule(ComponentType::Strategy, progId, &stage);
    if (errors.empty()) {
        std::fprintf(stdout, "    verifyModule: no errors (unexpected)\n");
        st->exitCode = fail(
            "verifyModule succeeded on invalid source -- test source is not "
            "actually invalid",
            40);
        // Clean up the (now-valid) entry.
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }
    std::fprintf(stdout, "    verifyModule: compilation FAILED (expected)\n");
    std::fprintf(stderr, "    errors:\n%s\n", errors.c_str());
    std::fprintf(stdout, "    outStage = %d (expect %d = VerifyModule_CompileErrors)\n",
                 static_cast<int>(stage),
                 static_cast<int>(tsdev::CreatePathFailureStage::VerifyModule_CompileErrors));
    if (stage != tsdev::CreatePathFailureStage::VerifyModule_CompileErrors) {
        st->exitCode = fail(
            "verifyModule did NOT set outStage=VerifyModule_CompileErrors "
            "on the compile-error path",
            41);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }

    // --- Step 6: findExistPos — ASSERT entry STILL EXISTS (verifyModule
    //      no longer auto-cleans up — TVS-L.10 moved cleanup to doInsert) ---
    std::fprintf(stdout,
                 "\n[6] findExistPos(\"%s\", Strategy) -- "
                 "asserting entry STILL EXISTS (verifyModule no longer cleans)...\n",
                 kTestName);
    int afterVerifyProgId = lib->findExistPos(kTestName, ComponentType::Strategy);
    std::fprintf(stdout, "    findExistPos = %d\n", afterVerifyProgId);
    if (afterVerifyProgId <= 0) {
        // verifyModule cleaned up — that is the OLD TD-001 behavior, which
        // TVS-L.10 removed. Failing here catches a regression that re-adds
        // cleanup inside verifyModule.
        st->exitCode = fail(
            "verifyModule auto-cleaned the partial entry -- TVS-L.10 moved "
            "cleanup to doInsert; verifyModule must NOT clean up",
            50);
        delete lib;
        return 0;
    }
    std::fprintf(stdout,
                 "    PASS: partial entry still exists after verifyModule "
                 "(cleanup is the caller's job)\n");

    // --- Step 7: Explicit deleteEntry — simulates what doInsert does on a
    //      post-createEntry failure. Removes the warehouse entry + both
    //      artifacts (.ell/.xml). Assert the entry is gone afterwards. ---
    std::fprintf(stdout,
                 "\n[7] deleteEntry(Strategy, \"%s\", flush=false) -- "
                 "simulating doInsert cleanup...\n",
                 kTestName);
    bool cleaned = lib->deleteEntry(ComponentType::Strategy, kTestName,
                                    /*flush=*/false);
    std::fprintf(stdout, "    deleteEntry = %s\n", cleaned ? "true" : "false");
    if (!cleaned) {
        st->exitCode = fail(
            "deleteEntry returned false -- could not remove the partial entry",
            60);
        // Best-effort retry with flush=true (single-item mode).
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }

    int afterCleanupProgId = lib->findExistPos(kTestName, ComponentType::Strategy);
    std::fprintf(stdout, "    findExistPos after cleanup = %d\n",
                 afterCleanupProgId);
    if (afterCleanupProgId > 0) {
        st->exitCode = fail(
            "partial entry STILL exists after explicit deleteEntry cleanup",
            70);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }

    std::fprintf(stdout,
                 "\nPASS: TVS-L.10 contract holds -- verifyModule reports stage "
                 "without cleaning; explicit caller cleanup removes the entry\n");
    st->exitCode = 0;

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_auto_cleanup: TVS-L.10 create-path cleanup contract "
                 "(verifyModule reports stage; caller owns cleanup) ===\n");
    std::fprintf(stdout, "Strategy name: %s\n", kTestName);

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

    std::fprintf(stdout, "\n=== test_auto_cleanup complete (exit %d) ===\n",
                 st.exitCode);
    return st.exitCode;
}
