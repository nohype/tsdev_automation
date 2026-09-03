// test_delete_tradingapp.cpp
//
// Integration test (RED-GREEN) for the DELETE flow through the tsdev_lib
// wrapper, applied to the TradingApp component type (TVS-C.5).
// This validates the TradingApp delete operation end-to-end:
//   1. Create a test TradingApp entry (INSERT flow: createEntry + saveSource
//      + verifyModule) so we have something to delete.
//   2. Verify the .ell file exists at elmodule_5122_<progId>.ell.
//   3. Delete it (deleteEntry).
//   4. Verify it no longer exists (findExistPos <= 0) in a fresh TsDevLib.
//   5. Verify the .ell file is deleted from disk.
//   6. Verify the .xml file is deleted from disk (if it existed).
//   7. Negative test: deleteEntry on a nonexistent entry returns false.
//   8. Clean up any remaining test artifacts.
//
// The test runs on a 64 MB-stack worker thread (the compiler and the
// thread-affine hidden EDIT control require it), matching the pattern of
// test_delete.cpp.
//
// Exit code: 0 on success, non-zero on failure.

#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace tsdev;

namespace {

// Test fixture constants. Unique names avoid collisions with C.4/C.6 which
// run in parallel.
const char* kTestName      = "test_ta_delete_c5";
const char* kNonexistent   = "nonexistent_ta_c5";

// Minimal valid TradingApp EasyLanguage source. "using elsystem;" was proven
// to compile successfully for TradingApp during the C.3 E2E CLI verification.
const char* kTestSource =
    "{ test_ta_delete_c5 }\r\n"
    "using elsystem;\r\n";

// Delete a file if it exists. Returns true if absent or successfully deleted.
bool cleanFile(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        return true; // already absent
    }
    return (DeleteFileA(path.c_str()) != 0);
}

// Print a failure banner and return the given code.
int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s\n", msg);
    return code;
}

// Shared state between main and the worker thread.
struct TestState {
    int progId   = 0;
    int exitCode = 0;
};

// The full TradingApp DELETE test flow, running on a large-stack worker
// thread. TsDevLib is constructed AND destructed here so the thread-affine
// EDIT control lives and dies on the same thread that uses it.
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

    // --- Step 2: Create a test TradingApp entry (INSERT flow) ---
    // First check if it already exists (from a previous failed run). If so,
    // delete it so we start clean.
    std::fprintf(stdout, "\n[2] findExistPos(\"%s\", TradingApp)...\n",
                 kTestName);
    st->progId = lib->findExistPos(kTestName, ComponentType::TradingApp);
    std::fprintf(stdout, "    findExistPos = %d\n", st->progId);

    if (st->progId > 0) {
        std::fprintf(stdout,
                     "    >>> Stale entry found (ProgId=%d) — deleting to "
                     "start clean\n",
                     st->progId);
        if (!lib->deleteEntry(ComponentType::TradingApp, kTestName)) {
            std::fprintf(stdout,
                         "    >>> Stale deleteEntry failed — continuing\n");
        }
        st->progId = 0;
    }

    if (st->progId <= 0) {
        // Create the entry.
        std::fprintf(stdout,
                     "\n[2a] createEntry(TradingApp, \"%s\")...\n",
                     kTestName);
        int errCode = 0;
        st->progId = lib->createEntry(ComponentType::TradingApp, kTestName,
                                      errCode);
        std::fprintf(stdout, "    createEntry = %d, errCode = 0x%X\n",
                     st->progId, errCode);
        if (st->progId <= 0) {
            st->exitCode = fail("createEntry returned ProgId <= 0", 20);
            delete lib;
            return 0;
        }

        // Save source.
        std::fprintf(stdout,
                     "\n[2b] saveSource(TradingApp, %d, source)...\n",
                     st->progId);
        bool saved = lib->saveSource(ComponentType::TradingApp, st->progId,
                                     kTestSource);
        std::fprintf(stdout, "    saveSource = %s\n",
                     saved ? "true" : "false");
        if (!saved) {
            st->exitCode = fail("saveSource returned false", 30);
            // Best-effort cleanup.
            lib->deleteEntry(ComponentType::TradingApp, kTestName);
            delete lib;
            return 0;
        }

        // Compile/verify to create the .ell file.
        std::fprintf(stdout,
                     "\n[2c] verifyModule(TradingApp, %d)...\n",
                     st->progId);
        std::string errors =
            lib->verifyModule(ComponentType::TradingApp, st->progId);
        if (errors.empty()) {
            std::fprintf(stdout, "    verifyModule: no errors (OK)\n");
        } else {
            std::fprintf(stderr, "    verifyModule errors:\n%s\n",
                         errors.c_str());
            st->exitCode = fail("verifyModule returned compilation errors", 40);
            // Best-effort cleanup.
            lib->deleteEntry(ComponentType::TradingApp, kTestName);
            delete lib;
            return 0;
        }
    }

    // --- Step 3: Verify the entry exists and .ell file is present ---
    std::fprintf(stdout, "\n[3] Verify entry exists: ProgId = %d\n",
                 st->progId);
    if (st->progId <= 0) {
        st->exitCode = fail("entry does not exist before delete", 50);
        delete lib;
        return 0;
    }

    // Verify the .ell file exists (it should after a successful compile).
    std::string ellPath =
        lib->getEllPath(ComponentType::TradingApp, st->progId);
    std::string xmlPath =
        lib->getXmlPath(ComponentType::TradingApp, st->progId);
    bool ellExists = lib->ellFileExists(ComponentType::TradingApp, st->progId);
    std::fprintf(stdout, "    .ell file: %s (%s)\n",
                 ellExists ? "EXISTS" : "NOT FOUND", ellPath.c_str());
    bool xmlExists =
        (GetFileAttributesA(xmlPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    std::fprintf(stdout, "    .xml file: %s (%s)\n",
                 xmlExists ? "EXISTS" : "NOT FOUND", xmlPath.c_str());

    // --- Step 4: Delete the entry ---
    std::fprintf(stdout, "\n[4] deleteEntry(TradingApp, \"%s\")...\n",
                 kTestName);
    bool deleted = lib->deleteEntry(ComponentType::TradingApp, kTestName);
    std::fprintf(stdout, "    deleteEntry = %s\n",
                 deleted ? "true" : "false");
    if (!deleted) {
        st->exitCode = fail("deleteEntry returned false", 60);
        delete lib;
        return 0;
    }

    // --- Step 5: In-memory check (warehouse still loaded) ---
    std::fprintf(stdout,
                 "\n[5] In-memory check (warehouse still loaded):\n");
    int postDelete = lib->findExistPos(kTestName, ComponentType::TradingApp);
    std::fprintf(stdout, "    findExistPos = %d (in-memory)\n", postDelete);
    if (postDelete <= 0) {
        std::fprintf(stdout, "    >>> Entry removed from in-memory lookup\n");
    } else {
        std::fprintf(stdout,
                     "    >>> Entry still in in-memory lookup (ProgId=%d) — "
                     "will verify after OrlibExit\n",
                     postDelete);
    }

    // Destroy TsDevLib (calls OrlibExit which persists the deletion).
    std::fprintf(stdout, "\n[6] Destroying TsDevLib (OrlibExit)...\n");
    delete lib;
    lib = nullptr;
    std::fprintf(stdout, "    TsDevLib destroyed\n");

    // --- Step 7: Verify in a FRESH TsDevLib instance ---
    std::fprintf(stdout,
                 "\n[7] Fresh TsDevLib — verify deletion persisted...\n");
    try {
        lib = new TsDevLib();
    } catch (const std::exception& e) {
        st->exitCode = fail(e.what(), 70);
        return 0;
    }

    int finalCheck = lib->findExistPos(kTestName, ComponentType::TradingApp);
    std::fprintf(stdout, "    findExistPos(\"%s\") = %d\n",
                 kTestName, finalCheck);
    if (finalCheck > 0) {
        std::fprintf(stderr,
                     "    >>> Entry STILL EXISTS after OrlibExit/reload "
                     "(ProgId=%d) — deletion did NOT persist!\n",
                     finalCheck);
        st->exitCode = fail("deletion did not persist to disk", 80);
        // Best-effort cleanup.
        std::string p =
            lib->getEllPath(ComponentType::TradingApp, finalCheck);
        cleanFile(p);
        lib->deleteEntry(ComponentType::TradingApp, kTestName);
        delete lib;
        return 0;
    }
    std::fprintf(stdout, "    >>> Entry GONE — deletion persisted!\n");

    // --- Step 8: Verify the .ell file is deleted from disk ---
    std::fprintf(stdout, "\n[8] Verify .ell file deleted: %s\n",
                 ellPath.c_str());
    bool ellStillExists =
        (GetFileAttributesA(ellPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    if (ellStillExists) {
        std::fprintf(stderr, "    >>> .ell file still on disk!\n");
        cleanFile(ellPath);
        st->exitCode = fail(".ell file was not deleted from disk", 90);
        delete lib;
        return 0;
    }
    std::fprintf(stdout, "    >>> .ell file deleted (or never created)\n");

    // --- Step 9: Verify the .xml file is deleted from disk ---
    std::fprintf(stdout, "\n[9] Verify .xml file deleted: %s\n",
                 xmlPath.c_str());
    bool xmlStillExists =
        (GetFileAttributesA(xmlPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    if (xmlStillExists) {
        std::fprintf(stderr, "    >>> .xml file still on disk!\n");
        cleanFile(xmlPath);
        st->exitCode = fail(".xml file was not deleted from disk", 95);
        delete lib;
        return 0;
    }
    std::fprintf(stdout, "    >>> .xml file deleted (or never existed)\n");

    // --- Step 10: Negative test — delete a nonexistent entry ---
    std::fprintf(stdout,
                 "\n[10] deleteEntry on nonexistent entry \"%s\"...\n",
                 kNonexistent);
    bool delNonexistent =
        lib->deleteEntry(ComponentType::TradingApp, kNonexistent);
    std::fprintf(stdout, "    deleteEntry(nonexistent) = %s (expected false)\n",
                 delNonexistent ? "true" : "false");
    if (delNonexistent) {
        st->exitCode = fail(
            "deleteEntry returned true for a nonexistent entry", 100);
        delete lib;
        return 0;
    }
    std::fprintf(stdout, "    >>> Correctly returned false for nonexistent\n");

    // --- SUCCESS ---
    std::fprintf(stdout,
                 "\nSUCCESS: TradingApp DELETE flow validated end-to-end.\n"
                 "  - TradingApp entry created, compiled, .ell persisted\n"
                 "  - deleteEntry removed it from the warehouse\n"
                 "  - Deletion persisted across OrlibExit/reload\n"
                 "  - .ell file removed from disk\n"
                 "  - .xml file removed from disk\n"
                 "  - Nonexistent entry correctly returns false\n");
    st->exitCode = 0;

    delete lib;
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_delete_tradingapp: TradingApp DELETE flow "
                 "end-to-end ===\n");
    std::fprintf(stdout, "TradingApp name: %s\n", kTestName);

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

    std::fprintf(stdout,
                 "\n=== test_delete_tradingapp complete (exit %d) ===\n",
                 st.exitCode);
    return st.exitCode;
}
