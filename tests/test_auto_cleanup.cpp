// test_auto_cleanup.cpp
//
// Integration test (GREEN — fix implemented in tsdev_lib.cpp) for TD-001:
//   "Partial warehouse entry on compile failure — no auto-cleanup."
//
// When `tsdev_cli insert` fails compilation (exit 8), `createEntry` has
// already allocated a ProgId and created a warehouse entry. The CLI now
// auto-cleans up this partial entry (fix implemented in tsdev_lib.cpp).
//
// This test verifies the DESIRED behavior: after a failed insert (compile
// error), the partial entry SHOULD be cleaned up automatically. It replicates
// the insert flow (createEntry + saveSource + verifyModule) with invalid
// EasyLanguage source that fails compilation, then asserts that the partial
// entry does NOT exist afterwards (findExistPos returns <= 0).
//
// The auto-cleanup is implemented, so this test PASSES (GREEN).
//
// The test is self-cleaning: if the partial entry persists (test fails), it
// deletes the entry via deleteEntry so the test is repeatable.
//
// Flow:
//   1. Construct TsDevLib (loads orlib20.dll, patches, inits, EDIT control)
//   2. findExistPos(name, Strategy) — delete if it already exists (pre-clean)
//   3. createEntry(Strategy, name, errCode) — assert ProgId > 0
//   4. saveSource(Strategy, progId, invalidSource) — assert success
//   5. verifyModule(Strategy, progId) — assert compilation FAILS (error string
//      non-empty); this is the compile-failure scenario
//   6. findExistPos(name, Strategy) — ASSERT returns <= 0 (entry should NOT
//      exist — auto-cleanup expected)
//      - If > 0: test FAILS — partial entry persists; clean it up
//   7. Print PASS / FAIL
//
// Exit code: 0 on PASS (entry auto-cleaned up), 1 on FAIL (entry persists).
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
// compiler will reject. The leading comment names the strategy (TSDev
// convention), matching the valid-source pattern from test_insert.cpp but with
// a deliberately broken body.
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
                     "    Entry already exists (ProgId=%d) — deleting...\n",
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

    // --- Step 5: verifyModule — expect compilation FAILURE ---
    std::fprintf(stdout, "\n[5] verifyModule(Strategy, %d)...\n", progId);
    std::string errors =
        lib->verifyModule(ComponentType::Strategy, progId);
    if (errors.empty()) {
        std::fprintf(stdout, "    verifyModule: no errors (unexpected)\n");
        st->exitCode = fail(
            "verifyModule succeeded on invalid source — test source is not "
            "actually invalid",
            40);
        // Clean up the (now-valid) entry.
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    } else {
        std::fprintf(stdout, "    verifyModule: compilation FAILED (expected)\n");
        std::fprintf(stderr, "    errors:\n%s\n", errors.c_str());
    }

    // --- Step 6: findExistPos — ASSERT entry does NOT exist (auto-cleanup) ---
    std::fprintf(stdout,
                 "\n[6] findExistPos(\"%s\", Strategy) — "
                 "asserting entry does NOT exist...\n",
                 kTestName);
    int afterProgId = lib->findExistPos(kTestName, ComponentType::Strategy);
    std::fprintf(stdout, "    findExistPos = %d\n", afterProgId);

    if (afterProgId <= 0) {
        // --- Step 7: PASS — entry was auto-cleaned up ---
        std::fprintf(stdout,
                     "\nPASS: partial entry auto-cleaned up on compile "
                     "failure\n");
        st->exitCode = 0;
    } else {
        // --- Step 7: FAIL — partial entry persists ---
        std::fprintf(stdout,
                     "\nFAIL: partial entry persists after compile failure "
                     "(ProgId=%d)\n",
                     afterProgId);
        st->exitCode = fail(
            "partial warehouse entry NOT cleaned up after compile failure",
            1);
        // Clean up the persistent partial entry so the test is repeatable.
        std::fprintf(stdout,
                     "    Cleaning up persistent entry via deleteEntry...\n");
        bool cleaned = lib->deleteEntry(ComponentType::Strategy, kTestName);
        std::fprintf(stdout, "    deleteEntry = %s\n",
                     cleaned ? "true" : "false");
    }

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_auto_cleanup: partial entry cleanup on compile "
                 "failure (TD-001) ===\n");
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
