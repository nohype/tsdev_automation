// test_upsert.cpp
//
// Integration test (RED-GREEN) for the library-level primitives that the
// `upsert` CLI subcommand will compose. Upsert = "insert or update": if the
// item does not exist, insert it; if it does exist, modify it in place.
//
// TVS-F.3 subtask 1a (TDD scaffold): this test exercises the library-level
// composition (findExistPos + createEntry + saveSource + verifyModule +
// modifyEntry + deleteEntry) that the future `upsert` CLI subcommand will
// wrap. All of these primitives are already implemented in tsdev_lib.cpp, so
// the library-level assertions in this test PASS today. The test establishes
// the test infrastructure for the upsert feature; the CLI-level E2E test is
// a separate subtask.
//
// The KEY upsert invariant verified here: after a modifyEntry, the ProgId
// must NOT change (modification reuses the existing ProgId — no
// createEntry). This is what makes upsert safe: the warehouse entry identity
// is preserved across the update.
//
// The test is self-cleaning: it inserts a probe strategy, verifies the
// upsert composition, then deletes the probe.
//
// Flow:
//   1. Construct TsDevLib (loads orlib20.dll, patches, inits, EDIT control)
//   2. Pre-clean: findExistPos(name, Strategy) — delete if it already exists
//   3. findExistPos(name, Strategy) — ASSERT returns 0 (item does NOT exist)
//   4. createEntry(Strategy, name, errCode) — assert ProgId > 0
//   5. saveSource(Strategy, progId, sourceA) — assert success
//   6. verifyModule(Strategy, progId) — assert no errors
//   7. findExistPos(name, Strategy) — ASSERT returns the SAME ProgId (found)
//   8. modifyEntry(Strategy, name, sourceB) — assert success (Buy -> Sell)
//   9. findExistPos(name, Strategy) — ASSERT returns the SAME ProgId
//      (KEY upsert invariant: ProgId preserved after modify)
//  10. deleteEntry(Strategy, name) — cleanup
//  11. closeWarehouse() — best-effort flush
//  12. Print PASS / FAIL
//
// Exit code: 0 on success, nonzero on failure.
//
// NOTE: The EasyLanguage compiler (invoked via VerifyModule_New) requires a
// large stack, AND the hidden EDIT control created by TsDevLib is a Win32
// window (thread-affine). The reference probe (archive/elevated_test20.cpp) runs its
// ENTIRE flow — including orlib init and window creation — in a worker thread
// with a 64 MB stack. We do the same: TsDevLib is constructed AND destructed
// inside the worker thread. An SEH wrapper around the worker thread body
// catches any unhandled structured exception and converts it to a nonzero
// exit code.

#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace tsdev;

namespace {

// Test fixture constants.
// A unique name minimizes collision risk with other tests. The "zzz_" prefix
// sorts it to the end of the warehouse and signals a test entry.
const char* kTestName = "zzz_test_upsert_probe";

// Minimal valid EasyLanguage strategy source (initial). In EasyLanguage
// `{ ... }` is a multi-line COMMENT block, so the actual code must be OUTSIDE
// it. The leading comment names the strategy (TSDev convention); the body
// issues a simple market buy order. This mirrors the proven source from
// test_exists.cpp.
const char* kSourceA =
    "{ zzz_test_upsert_probe }\r\n"
    "Buy (\"EnterLong\") next bar at Market;\r\n";

// Modified source (source B): changes the strategy from Buy to Sell. This is
// the "update" half of the upsert — the same modification pattern used in
// test_modify.cpp.
const char* kSourceB =
    "{ zzz_test_upsert_probe }\r\n"
    "Sell (\"EnterShort\") next bar at Market;\r\n";

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

    // --- Step 3: findExistPos for a NONEXISTENT name — ASSERT returns 0 ---
    std::fprintf(stdout,
                 "\n[3] findExistPos(\"%s\", Strategy) — "
                 "asserting item does NOT exist...\n",
                 kTestName);
    int notFoundProgId = lib->findExistPos(kTestName,
                                           ComponentType::Strategy);
    std::fprintf(stdout, "    findExistPos = %d\n", notFoundProgId);
    if (notFoundProgId > 0) {
        st->exitCode = fail(
            "findExistPos returned a positive ProgId for a nonexistent item",
            20);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }
    std::fprintf(stdout,
                 "    PASS: nonexistent item not found (ProgId=%d)\n",
                 notFoundProgId);

    // --- Step 4: createEntry (the "insert" half of upsert) ---
    std::fprintf(stdout, "\n[4] createEntry(Strategy, \"%s\")...\n",
                 kTestName);
    int errCode = 0;
    int progId = lib->createEntry(ComponentType::Strategy, kTestName, errCode);
    std::fprintf(stdout, "    createEntry = %d, errCode = 0x%X\n",
                 progId, errCode);
    if (progId <= 0) {
        st->exitCode = fail("createEntry returned ProgId <= 0", 30);
        delete lib;
        return 0;
    }

    // --- Step 5: saveSource ---
    std::fprintf(stdout, "\n[5] saveSource(Strategy, %d, sourceA)...\n",
                 progId);
    bool saved = lib->saveSource(ComponentType::Strategy, progId,
                                 kSourceA);
    std::fprintf(stdout, "    saveSource = %s\n", saved ? "true" : "false");
    if (!saved) {
        st->exitCode = fail("saveSource returned false", 40);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }

    // --- Step 6: verifyModule (compile + persist) ---
    std::fprintf(stdout, "\n[6] verifyModule(Strategy, %d)...\n", progId);
    std::string errors =
        lib->verifyModule(ComponentType::Strategy, progId);
    if (errors.empty()) {
        std::fprintf(stdout, "    verifyModule: no errors (compilation OK)\n");
    } else {
        std::fprintf(stderr, "    verifyModule errors:\n%s\n",
                     errors.c_str());
        st->exitCode = fail("verifyModule returned compilation errors", 50);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }

    // --- Step 7: findExistPos for the EXISTING name — ASSERT same ProgId ---
    std::fprintf(stdout,
                 "\n[7] findExistPos(\"%s\", Strategy) — "
                 "asserting item EXISTS with ProgId=%d...\n",
                 kTestName, progId);
    int foundProgId = lib->findExistPos(kTestName, ComponentType::Strategy);
    std::fprintf(stdout, "    findExistPos = %d\n", foundProgId);
    if (foundProgId <= 0) {
        st->exitCode = fail(
            "findExistPos returned <= 0 for an existing item", 60);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }
    if (foundProgId != progId) {
        std::fprintf(stderr,
                     "    FAIL: ProgId mismatch %d != %d\n",
                     foundProgId, progId);
        st->exitCode = fail(
            "findExistPos returned a different ProgId after insert", 65);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }
    std::fprintf(stdout, "    PASS: existing item found (ProgId=%d)\n",
                 foundProgId);

    // --- Step 8: modifyEntry (the "update" half of upsert) ---
    std::fprintf(stdout,
                 "\n[8] modifyEntry(Strategy, \"%s\", sourceB)...\n",
                 kTestName);
    Result modRes = lib->modifyEntry(ComponentType::Strategy, kTestName,
                                     kSourceB);
    std::fprintf(stdout, "    modifyEntry: success=%d message=\"%s\"\n",
                 (int)modRes.success, modRes.message.c_str());
    if (!modRes.success) {
        st->exitCode = fail("modifyEntry returned failure on valid source B",
                            70);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }

    // --- Step 9: findExistPos again — ASSERT SAME ProgId (KEY invariant) ---
    std::fprintf(stdout,
                 "\n[9] findExistPos(\"%s\", Strategy) — "
                 "asserting SAME ProgId=%d after modify...\n",
                 kTestName, progId);
    int progIdAfter = lib->findExistPos(kTestName, ComponentType::Strategy);
    std::fprintf(stdout, "    findExistPos = %d (was %d)\n",
                 progIdAfter, progId);
    if (progIdAfter <= 0) {
        st->exitCode = fail(
            "findExistPos returned <= 0 after modify", 80);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }
    if (progIdAfter != progId) {
        std::fprintf(stderr,
                     "    FAIL: ProgId changed %d -> %d (must NOT change)\n",
                     progId, progIdAfter);
        st->exitCode = fail(
            "ProgId changed after modify (KEY upsert invariant violated)", 85);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }
    std::fprintf(stdout,
                 "    PASS: ProgId preserved after modify (%d)\n",
                 progIdAfter);

    // --- Step 10: Cleanup — delete the probe entry ---
    std::fprintf(stdout,
                 "\n[10] Cleanup: deleteEntry(Strategy, \"%s\")...\n",
                 kTestName);
    bool deleted = lib->deleteEntry(ComponentType::Strategy, kTestName);
    std::fprintf(stdout, "    deleteEntry = %s\n",
                 deleted ? "true" : "false");
    if (!deleted) {
        st->exitCode = fail("cleanup deleteEntry failed", 90);
    }

    // --- Step 11: closeWarehouse (best-effort flush) ---
    std::fprintf(stdout, "\n[11] closeWarehouse()...\n");
    lib->closeWarehouse();
    std::fprintf(stdout, "    closeWarehouse done\n");

    // --- Step 12: Result ---
    if (st->exitCode == 0) {
        std::fprintf(stdout,
                     "\nPASS: upsert composition verified — "
                     "insert + modify preserve ProgId\n");
    }

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
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
                 "=== test_upsert: upsert composition (TVS-F.3) ===\n");
    std::fprintf(stdout, "Strategy name: %s\n", kTestName);

    // Run the entire test on a worker thread with a 64 MB stack. The
    // EasyLanguage compiler needs far more than the default 1 MB, and the
    // hidden EDIT control is thread-affine so TsDevLib must live on this
    // thread.
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

    std::fprintf(stdout, "\n=== test_upsert complete (exit %d) ===\n",
                 st.exitCode);
    return st.exitCode;
}
