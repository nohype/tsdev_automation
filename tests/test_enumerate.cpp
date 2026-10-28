// test_enumerate.cpp
//
// Integration test for the current warehouse/API-backed `enumerateRecords`
// behavior of TsDevLib.
//
// TVS-F.5 subtask F5-S2: this test exercises the library-level
// case-insensitive existence-query primitive that the `exists
// --case-insensitive` CLI subcommand wraps.
//
// The test is self-cleaning: it inserts a probe strategy, queries
// enumerateRecords for the existing name in UPPERCASE (expects exists=true,
// verbatim stored name, ProgId > 0) and for a nonexistent name (expects
// exists=false, empty stored name, ProgId == 0), then deletes the probe.
//
// Flow (mirrors tests/test_exists.cpp, per F5-S2 plan):
//   1. Construct TsDevLib (loads orlib20.dll, patches, inits, EDIT control)
//   2. Pre-clean: findExistPos(name, Strategy) — delete if it already exists
//   3. createEntry(Strategy, name, errCode) — assert ProgId > 0
//   4. saveSource(Strategy, progId, source) — assert success
//   5. verifyModule(Strategy, progId) — assert no errors
//   6. enumerateRecords(Strategy, nameUpper) — ASSERT exists == true,
//      storedName == kTestName (verbatim, correct case), progId > 0
//   7. enumerateRecords(Strategy, "NONEXISTENTNAMEXYZ") — ASSERT exists ==
//      false, storedName == "", progId == 0
//   8. deleteEntry(Strategy, name) — cleanup
//   9. Print PASS / FAIL
//
// Exit code: 0 on success, nonzero on failure.
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
const char* kTestName = "zzz_test_ci_probe";

// The same name converted to UPPERCASE — used to verify case-insensitive
// matching via enumerateRecords (the stored name is lowercase/mixed, the
// query is uppercase; the returned storedName must be the verbatim original).
const char* kTestNameUpper = "ZZZ_TEST_CI_PROBE";

// A name that is guaranteed not to exist in the warehouse.
const char* kNonExistentName = "NONEXISTENTNAMEXYZ";

// Minimal valid EasyLanguage strategy source. In EasyLanguage `{ ... }` is a
// multi-line COMMENT block, so the actual code must be OUTSIDE it. The brace
// text is arbitrary fixture source content only; it is not a metadata
// convention or lookup mechanism. The name and ProgId come from warehouse
// records/APIs. This mirrors the proven source from test_exists.cpp.
const char* kTestSource =
    "{ zzz_test_ci_probe }\r\n"
    "Buy (\"EnterLong\") next bar at Market;\r\n";

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

    // --- Step 4: saveSource ---
    std::fprintf(stdout, "\n[4] saveSource(Strategy, %d, source)...\n",
                 progId);
    bool saved = lib->saveSource(ComponentType::Strategy, progId,
                                 kTestSource);
    std::fprintf(stdout, "    saveSource = %s\n", saved ? "true" : "false");
    if (!saved) {
        st->exitCode = fail("saveSource returned false", 30);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }

    // --- Step 5: verifyModule (compile + persist) ---
    std::fprintf(stdout, "\n[5] verifyModule(Strategy, %d)...\n", progId);
    std::string errors =
        lib->verifyModule(ComponentType::Strategy, progId);
    if (errors.empty()) {
        std::fprintf(stdout, "    verifyModule: no errors (compilation OK)\n");
    } else {
        std::fprintf(stderr, "    verifyModule errors:\n%s\n",
                     errors.c_str());
        st->exitCode = fail("verifyModule returned compilation errors", 40);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }

    // --- Step 6: enumerateRecords for the EXISTING name (UPPERCASE query) ---
    // Asserts case-insensitive match: querying "ZZZ_TEST_CI_PROBE" must find
    // the item stored as "zzz_test_ci_probe" (verbatim stored name returned),
    // with exists == true and progId > 0.
    std::fprintf(stdout,
                 "\n[6] enumerateRecords(Strategy, \"%s\") -- "
                 "asserting case-insensitive MATCH...\n",
                 kTestNameUpper);
    EnumerateResult result =
        lib->enumerateRecords(ComponentType::Strategy, kTestNameUpper);
    std::fprintf(stdout,
                 "    enumerateRecords: exists=%d, storedName=\"%s\", "
                 "progId=%d\n",
                 result.exists ? 1 : 0, result.storedName.c_str(),
                 result.progId);
    if (!result.exists) {
        st->exitCode = fail(
            "enumerateRecords returned exists == false for an existing item "
            "(queried case-insensitively)",
            50);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }
    if (result.progId <= 0) {
        st->exitCode = fail(
            "enumerateRecords returned progId <= 0 for an existing item",
            51);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }
    // The stored name must be the VERBATIM (correctly-cased) stored name,
    // NOT the uppercase query. This is the key case-insensitive invariant.
    if (result.storedName != kTestName) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "enumerateRecords storedName mismatch: expected \"%s\" "
                      "(verbatim), got \"%s\"",
                      kTestName, result.storedName.c_str());
        st->exitCode = fail(buf, 52);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }
    std::fprintf(stdout,
                 "    PASS: case-insensitive match found (storedName=\"%s\", "
                 "progId=%d)\n",
                 result.storedName.c_str(), result.progId);

    // --- Step 7: enumerateRecords for a NONEXISTENT name — ASSERT not found ---
    std::fprintf(stdout,
                 "\n[7] enumerateRecords(Strategy, \"%s\") -- "
                 "asserting item does NOT exist...\n",
                 kNonExistentName);
    EnumerateResult notFound =
        lib->enumerateRecords(ComponentType::Strategy, kNonExistentName);
    std::fprintf(stdout,
                 "    enumerateRecords: exists=%d, storedName=\"%s\", "
                 "progId=%d\n",
                 notFound.exists ? 1 : 0, notFound.storedName.c_str(),
                 notFound.progId);
    if (notFound.exists) {
        st->exitCode = fail(
            "enumerateRecords returned exists == true for a nonexistent item",
            60);
        // Still clean up the probe entry below.
    } else if (notFound.progId != 0) {
        st->exitCode = fail(
            "enumerateRecords returned nonzero progId for a nonexistent item",
            61);
    } else if (!notFound.storedName.empty()) {
        st->exitCode = fail(
            "enumerateRecords returned non-empty storedName for a nonexistent "
            "item",
            62);
    } else {
        std::fprintf(stdout,
                     "    PASS: nonexistent item not found (exists=false, "
                     "storedName=\"\", progId=0)\n");
    }

    // --- Step 8: Cleanup — delete the probe entry ---
    std::fprintf(stdout, "\n[8] Cleanup: deleteEntry(Strategy, \"%s\")...\n",
                 kTestName);
    bool deleted = lib->deleteEntry(ComponentType::Strategy, kTestName);
    std::fprintf(stdout, "    deleteEntry = %s\n",
                 deleted ? "true" : "false");
    if (!deleted) {
        st->exitCode = fail("cleanup deleteEntry failed", 70);
    }

    // --- Step 9: Result ---
    if (st->exitCode == 0) {
        std::fprintf(stdout,
                     "\nPASS: enumerateRecords correctly reports "
                     "case-insensitive existence and nonexistence\n");
    }

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_enumerate: enumerateRecords case-insensitive query "
                 "(TVS-F.5) ===\n");
    std::fprintf(stdout, "Strategy name: %s\n", kTestName);
    std::fprintf(stdout, "Upper query  : %s\n", kTestNameUpper);
    std::fprintf(stdout, "Nonexistent  : %s\n", kNonExistentName);

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

    std::fprintf(stdout, "\n=== test_enumerate complete (exit %d) ===\n",
                 st.exitCode);
    return st.exitCode;
}
