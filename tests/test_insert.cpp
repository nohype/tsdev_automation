// test_insert.cpp
//
// Integration test (RED-GREEN) for the INSERT flow through the tsdev_lib
// wrapper. This is the first end-to-end validation of the shared wrapper
// module: it constructs a TsDevLib instance, creates a minimal valid
// EasyLanguage strategy, compiles it, persists the .ell file, and verifies
// the file exists on disk.
//
// The test is self-cleaning: it deletes the test entry's .ell file before
// and after the run so it can be repeated idempotently.
//
// Flow (mirrors orlib_param_orders.md §15):
//   1. Construct TsDevLib (loads orlib20.dll, patches, inits, EDIT control)
//   2. findExistPos(name, Strategy) — skip if already present (re-run ok)
//   3. createEntry(Strategy, name, errCode) — assert ProgId > 0
//   4. saveSource(Strategy, progId, source) — assert success
//   5. verifyModule(Strategy, progId) — assert error string empty
//   6. closeWarehouse()
//   7. ellFileExists(Strategy, progId) — assert true
//   8. Print SUCCESS with ProgId and .ell path
//
// Exit code: 0 on success, non-zero on failure.
//
// NOTE: The EasyLanguage compiler (invoked via VerifyModule_New) requires a
// large stack, AND the hidden EDIT control created by TsDevLib is a Win32
// window (thread-affine). The reference probe (elevated_test20.cpp) runs its
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
const char* kTestName = "zzz_cli_insert_t2";

// Minimal valid EasyLanguage strategy source. In EasyLanguage `{ ... }` is a
// multi-line COMMENT block, so the actual code must be OUTSIDE it. The leading
// comment names the strategy (TSDev convention); the body issues a simple
// market buy order. This mirrors the proven source from elevated_test20.cpp.
const char* kTestSource =
    "{ zzz_cli_insert_t2 }\r\n"
    "Buy (\"EnterLong\") next bar at Market;\r\n";

// Delete the .ell file for a given ProgId so the test is repeatable.
// Returns true if the file was absent or successfully deleted.
bool cleanEllFile(const std::string& path) {
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
    int progId    = 0;
    int exitCode  = 0;
};

// The full INSERT flow, running on a large-stack worker thread. TsDevLib is
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

    // --- Step 2: findExistPos — check if it already exists ---
    std::fprintf(stdout, "\n[2] findExistPos(\"%s\", Strategy)...\n",
                 kTestName);
    st->progId = lib->findExistPos(kTestName, ComponentType::Strategy);
    std::fprintf(stdout, "    findExistPos = %d\n", st->progId);

    bool alreadyExisted = (st->progId > 0);

    // --- Step 3: createEntry (only if not found) ---
    if (!alreadyExisted) {
        std::fprintf(stdout, "\n[3] createEntry(Strategy, \"%s\")...\n",
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
    } else {
        std::fprintf(stdout,
                     "\n[3] Component already exists (ProgId=%d) — "
                     "skipping createEntry (re-run)\n",
                     st->progId);
        // Pre-clean the .ell so we can verify re-creation.
        std::string ellPath =
            lib->getEllPath(ComponentType::Strategy, st->progId);
        cleanEllFile(ellPath);
    }

    // --- Step 4: saveSource ---
    std::fprintf(stdout, "\n[4] saveSource(Strategy, %d, source)...\n",
                 st->progId);
    bool saved = lib->saveSource(ComponentType::Strategy, st->progId,
                                 kTestSource);
    std::fprintf(stdout, "    saveSource = %s\n", saved ? "true" : "false");
    if (!saved) {
        st->exitCode = fail("saveSource returned false", 30);
        delete lib;
        return 0;
    }

    // --- Step 5: verifyModule (compile + persist) ---
    std::fprintf(stdout, "\n[5] verifyModule(Strategy, %d)...\n", st->progId);
    std::string errors =
        lib->verifyModule(ComponentType::Strategy, st->progId);
    if (errors.empty()) {
        std::fprintf(stdout,
                     "    verifyModule: no errors (compilation OK)\n");
    } else {
        std::fprintf(stderr, "    verifyModule errors:\n%s\n",
                     errors.c_str());
        st->exitCode = fail("verifyModule returned compilation errors", 40);
        delete lib;
        return 0;
    }

    // --- Step 6: closeWarehouse ---
    std::fprintf(stdout, "\n[6] closeWarehouse()...\n");
    lib->closeWarehouse();
    std::fprintf(stdout, "    closeWarehouse done\n");

    // --- Step 7: ellFileExists ---
    std::string ellPath =
        lib->getEllPath(ComponentType::Strategy, st->progId);
    std::fprintf(stdout, "\n[7] ellFileExists: %s\n", ellPath.c_str());
    bool exists = lib->ellFileExists(ComponentType::Strategy, st->progId);
    std::fprintf(stdout, "    exists = %s\n", exists ? "true" : "false");
    if (!exists) {
        st->exitCode = fail(".ell file was not created on disk", 50);
        // Still clean up below before deleting lib.
    } else {
        // --- Step 8: SUCCESS ---
        std::fprintf(stdout,
                     "\nSUCCESS: strategy \"%s\" inserted, ProgId=%d, .ell=%s\n",
                     kTestName, st->progId, ellPath.c_str());
    }

    // --- Post-clean: remove the .ell so the test is repeatable. ---
    if (st->progId > 0) {
        std::string p =
            lib->getEllPath(ComponentType::Strategy, st->progId);
        if (cleanEllFile(p)) {
            std::fprintf(stdout, "Cleanup: removed %s\n", p.c_str());
        } else {
            std::fprintf(stderr,
                         "Cleanup: could NOT remove %s (test not repeatable)\n",
                         p.c_str());
        }
    }

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout, "=== test_insert: INSERT flow end-to-end ===\n");
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

    std::fprintf(stdout, "\n=== test_insert complete (exit %d) ===\n",
                 st.exitCode);
    return st.exitCode;
}
