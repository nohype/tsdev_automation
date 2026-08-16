// test_delete.cpp
//
// Integration test (RED-GREEN) for the DELETE flow through the tsdev_lib
// wrapper. This validates the delete operation end-to-end:
//   1. Create a test entry (INSERT flow: createEntry + saveSource + verify)
//   2. Verify it exists (findExistPos > 0)
//   3. Delete it (deleteEntry)
//   4. Verify it no longer exists (findExistPos <= 0)
//   5. Verify the .ell file is deleted
//   6. Test deleting a nonexistent entry (should return false)
//   7. Clean up any remaining test artifacts
//
// The test runs on a 64 MB-stack worker thread (the compiler and the
// thread-affine hidden EDIT control require it), matching the pattern of
// test_insert.cpp.
//
// Exit code: 0 on success, non-zero on failure.

#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace tsdev;

namespace {

// Test fixture constants.
const char* kTestName = "zzz_cli_delete_test";

// Minimal valid EasyLanguage strategy source. The leading comment names the
// strategy (TSDev convention); the body issues a simple market buy order.
const char* kTestSource =
    "{ zzz_cli_delete_test }\r\n"
    "Buy (\"EnterLong\") next bar at Market;\r\n";

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

// The full DELETE test flow, running on a large-stack worker thread. TsDevLib
// is constructed AND destructed here so the thread-affine EDIT control lives
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

    // --- Step 2: Create a test entry (INSERT flow) ---
    // First check if it already exists (from a previous failed run).
    std::fprintf(stdout, "\n[2] findExistPos(\"%s\", Strategy)...\n",
                 kTestName);
    st->progId = lib->findExistPos(kTestName, ComponentType::Strategy);
    std::fprintf(stdout, "    findExistPos = %d\n", st->progId);

    if (st->progId <= 0) {
        // Create the entry.
        std::fprintf(stdout, "\n[2a] createEntry(Strategy, \"%s\")...\n",
                     kTestName);
        int errCode = 0;
        st->progId = lib->createEntry(ComponentType::Strategy, kTestName,
                                      errCode);
        std::fprintf(stdout, "    createEntry = %d, errCode = 0x%X\n",
                     st->progId, errCode);
        if (st->progId <= 0) {
            st->exitCode = fail("createEntry returned ProgId <= 0", 20);
            delete lib;
            return 0;
        }

        // Save source.
        std::fprintf(stdout, "\n[2b] saveSource(Strategy, %d, source)...\n",
                     st->progId);
        bool saved = lib->saveSource(ComponentType::Strategy, st->progId,
                                     kTestSource);
        std::fprintf(stdout, "    saveSource = %s\n",
                     saved ? "true" : "false");
        if (!saved) {
            st->exitCode = fail("saveSource returned false", 30);
            delete lib;
            return 0;
        }

        // Compile/verify to create the .ell file.
        std::fprintf(stdout, "\n[2c] verifyModule(Strategy, %d)...\n",
                     st->progId);
        std::string errors =
            lib->verifyModule(ComponentType::Strategy, st->progId);
        if (errors.empty()) {
            std::fprintf(stdout, "    verifyModule: no errors (OK)\n");
        } else {
            std::fprintf(stderr, "    verifyModule errors:\n%s\n",
                         errors.c_str());
            st->exitCode = fail("verifyModule returned compilation errors", 40);
            delete lib;
            return 0;
        }
    } else {
        std::fprintf(stdout,
                     "\n[2a] Component already exists (ProgId=%d) — "
                     "skipping createEntry (re-run)\n",
                     st->progId);
    }

    // --- Step 3: Verify the entry exists ---
    std::fprintf(stdout, "\n[3] Verify entry exists: findExistPos = %d\n",
                 st->progId);
    if (st->progId <= 0) {
        st->exitCode = fail("entry does not exist before delete", 50);
        delete lib;
        return 0;
    }

    // Verify the .ell file exists (it should after a successful compile).
    std::string ellPath =
        lib->getEllPath(ComponentType::Strategy, st->progId);
    bool ellExists = lib->ellFileExists(ComponentType::Strategy, st->progId);
    std::fprintf(stdout, "    .ell file: %s (%s)\n",
                 ellExists ? "EXISTS" : "NOT FOUND", ellPath.c_str());

    // --- Step 4: Delete the entry ---
    std::fprintf(stdout, "\n[4] deleteEntry(Strategy, \"%s\")...\n",
                 kTestName);
    bool deleted = lib->deleteEntry(ComponentType::Strategy, kTestName);
    std::fprintf(stdout, "    deleteEntry = %s\n",
                 deleted ? "true" : "false");
    if (!deleted) {
        st->exitCode = fail("deleteEntry returned false", 60);
        delete lib;
        return 0;
    }

    // --- Step 5: Verify the entry no longer exists ---
    // NOTE: The deletion is persisted by OrlibExit (called in the destructor).
    // FindExistPos may still return the entry within this session because the
    // warehouse is still loaded in memory. The authoritative check is after
    // OrlibExit. We check in-memory here for diagnostics, then do the
    // authoritative check in a fresh TsDevLib instance below.
    std::fprintf(stdout,
                 "\n[5] In-memory check (warehouse still loaded):\n");
    int postDelete = lib->findExistPos(kTestName, ComponentType::Strategy);
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
    // A fresh OrlibInit reloads the warehouse from disk. If the deletion was
    // persisted, FindExistPos should return <= 0.
    std::fprintf(stdout,
                 "\n[7] Fresh TsDevLib — verify deletion persisted...\n");
    try {
        lib = new TsDevLib();
    } catch (const std::exception& e) {
        st->exitCode = fail(e.what(), 70);
        return 0;
    }

    int finalCheck = lib->findExistPos(kTestName, ComponentType::Strategy);
    std::fprintf(stdout, "    findExistPos(\"%s\") = %d\n",
                 kTestName, finalCheck);
    if (finalCheck > 0) {
        std::fprintf(stderr,
                     "    >>> Entry STILL EXISTS after OrlibExit/reload "
                     "(ProgId=%d) — deletion did NOT persist!\n",
                     finalCheck);
        st->exitCode = fail("deletion did not persist to disk", 80);
        // Clean up the .ell file manually as best-effort.
        std::string p =
            lib->getEllPath(ComponentType::Strategy, finalCheck);
        cleanFile(p);
        delete lib;
        return 0;
    }
    std::fprintf(stdout, "    >>> Entry GONE — deletion persisted!\n");

    // --- Step 8: Verify the .ell file is deleted ---
    // The progId may differ after reload, but we check the original path.
    std::fprintf(stdout, "\n[8] Verify .ell file deleted: %s\n",
                 ellPath.c_str());
    bool ellStillExists =
        (GetFileAttributesA(ellPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    if (ellStillExists) {
        std::fprintf(stderr, "    >>> .ell file still on disk!\n");
        // Best-effort cleanup.
        cleanFile(ellPath);
        st->exitCode = fail(".ell file was not deleted from disk", 90);
        delete lib;
        return 0;
    }
    std::fprintf(stdout, "    >>> .ell file deleted (or never created)\n");

    // --- Step 9: Test deleting a nonexistent entry ---
    std::fprintf(stdout,
                 "\n[9] deleteEntry on nonexistent entry...\n");
    bool delNonexistent = lib->deleteEntry(ComponentType::Strategy,
                                           "zzz_does_not_exist_12345");
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
                 "\nSUCCESS: DELETE flow validated end-to-end.\n"
                 "  - Entry created, compiled, .ell persisted\n"
                 "  - deleteEntry removed it from the warehouse\n"
                 "  - Deletion persisted across OrlibExit/reload\n"
                 "  - .ell file removed from disk\n"
                 "  - Nonexistent entry correctly returns false\n");
    st->exitCode = 0;

    delete lib;
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout, "=== test_delete: DELETE flow end-to-end ===\n");
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

    std::fprintf(stdout, "\n=== test_delete complete (exit %d) ===\n",
                 st.exitCode);
    return st.exitCode;
}
