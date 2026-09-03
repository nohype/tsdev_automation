// test_insert_tradingapp.cpp
//
// Integration test (RED-GREEN TDD) for TVS-C.4: TradingApp INSERT flow through
// the tsdev_lib wrapper. This formalizes the end-to-end behavior that was
// confirmed via CLI E2E during TVS-C.3 (ProgId 407, elmodule_5122_407.ell).
//
// The test constructs a TsDevLib instance, creates a minimal TradingApp entry,
// saves source, compiles/verifies it, persists the .ell file, checks the file
// magic (RNAM), and then cleans up by deleting the entry.
//
// The test is self-cleaning: it deletes the test entry (and its .ell file)
// before and after the run so it can be repeated idempotently. If the test
// fails midway, it still attempts cleanup.
//
// Flow:
//   1. Construct TsDevLib (loads orlib20.dll, patches, inits, EDIT control)
//   2. Pre-clean: deleteEntry if a stale entry exists from a prior failed run
//   3. createEntry(TradingApp, "test_ta_insert_c4") — assert ProgId > 0
//   4. saveSource(TradingApp, progId, source) — assert success
//   5. verifyModule(TradingApp, progId) — assert error string empty
//   6. Verify .ell file exists at elmodule_5122_<progId>.ell
//   7. Verify .ell file magic is "RNAM" (first 4 bytes)
//   8. deleteEntry(TradingApp, "test_ta_insert_c4") — assert true
//   9. Exit 0 on success
//
// Exit code: 0 on success, non-zero on failure.
//
// NOTE: The EasyLanguage compiler requires a large stack, and the hidden EDIT
// control is thread-affine. TsDevLib is constructed AND destructed inside a
// worker thread with a 64 MB stack (same pattern as test_insert.cpp).

#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

using namespace tsdev;

namespace {

// Unique test name to avoid collisions with parallel tests (C.5, C.6).
const char* kTestName = "test_ta_insert_c4";

// Minimal TradingApp source proven to compile in the TVS-C.3 E2E CLI test.
const char* kTestSource = "using elsystem;\r\n";

// Full TradingApp candidate (from TVS-C.2) — used as fallback if the minimal
// source fails to compile.
const char* kFullSource =
    "using elsystem;\r\n"
    "using elsystem.windows.forms;\r\n"
    "vars: Form mainForm(null);\r\n"
    "method void AnalysisTechnique_Initialized( elsystem.Object sender, elsystem.InitializedEventArgs args )\r\n"
    "begin\r\n"
    "    mainForm = Form.Create(\"MyApp\", 200, 200);\r\n"
    "    mainForm.Show();\r\n"
    "end;\r\n";

// Print a failure banner and return the given code.
int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// Read the first 4 bytes of a file and compare to the expected magic.
// Returns true if the file exists and the magic matches.
bool checkFileMagic(const std::string& path, const char expected[4]) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4] = {0};
    size_t n = std::fread(buf, 1, 4, f);
    std::fclose(f);
    if (n < 4) return false;
    return std::memcmp(buf, expected, 4) == 0;
}

// Shared state between main and the worker thread.
struct TestState {
    int exitCode = 0;
};

// The full INSERT flow, running on a large-stack worker thread.
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

    // --- Step 2: Pre-clean (delete stale entry from a prior failed run) ---
    std::fprintf(stdout, "\n[2] Pre-clean: deleteEntry(TradingApp, \"%s\")...\n",
                 kTestName);
    lib->deleteEntry(ComponentType::TradingApp, kTestName);
    std::fprintf(stdout, "    Pre-clean done\n");

    // --- Step 3: createEntry ---
    std::fprintf(stdout, "\n[3] createEntry(TradingApp, \"%s\")...\n",
                 kTestName);
    int errCode = 0;
    int progId = lib->createEntry(ComponentType::TradingApp, kTestName,
                                  errCode);
    std::fprintf(stdout, "    createEntry = %d, errCode = 0x%X\n",
                 progId, errCode);
    if (progId <= 0) {
        st->exitCode = fail("createEntry returned ProgId <= 0", 20);
        delete lib;
        return 0;
    }

    // --- Step 4: saveSource ---
    std::fprintf(stdout, "\n[4] saveSource(TradingApp, %d, source)...\n",
                 progId);
    bool saved = lib->saveSource(ComponentType::TradingApp, progId,
                                 kTestSource);
    std::fprintf(stdout, "    saveSource = %s\n", saved ? "true" : "false");
    if (!saved) {
        st->exitCode = fail("saveSource returned false", 30);
        lib->deleteEntry(ComponentType::TradingApp, kTestName);
        delete lib;
        return 0;
    }

    // --- Step 5: verifyModule (compile + persist) ---
    std::fprintf(stdout, "\n[5] verifyModule(TradingApp, %d)...\n", progId);
    std::string errors =
        lib->verifyModule(ComponentType::TradingApp, progId);
    if (errors.empty()) {
        std::fprintf(stdout, "    verifyModule: no errors (compilation OK)\n");
    } else {
        std::fprintf(stderr, "    verifyModule errors:\n%s\n",
                     errors.c_str());
        // Fallback: try the full TradingApp source.
        std::fprintf(stdout, "    Retrying with full TradingApp source...\n");
        lib->saveSource(ComponentType::TradingApp, progId, kFullSource);
        errors = lib->verifyModule(ComponentType::TradingApp, progId);
        if (errors.empty()) {
            std::fprintf(stdout,
                         "    verifyModule (full source): no errors\n");
        } else {
            std::fprintf(stderr, "    verifyModule (full source) errors:\n%s\n",
                         errors.c_str());
            st->exitCode = fail("verifyModule returned compilation errors", 40);
            lib->deleteEntry(ComponentType::TradingApp, kTestName);
            delete lib;
            return 0;
        }
    }

    // --- Step 6: Verify .ell file exists ---
    std::string ellPath = lib->getEllPath(ComponentType::TradingApp, progId);
    std::fprintf(stdout, "\n[6] .ell path: %s\n", ellPath.c_str());
    bool exists = lib->ellFileExists(ComponentType::TradingApp, progId);
    std::fprintf(stdout, "    exists = %s\n", exists ? "true" : "false");
    if (!exists) {
        st->exitCode = fail(".ell file was not created on disk", 50);
        lib->deleteEntry(ComponentType::TradingApp, kTestName);
        delete lib;
        return 0;
    }

    // --- Step 7: Verify .ell file magic is "RNAM" ---
    std::fprintf(stdout, "\n[7] Checking .ell magic (expect RNAM)...\n");
    if (!checkFileMagic(ellPath, "RNAM")) {
        st->exitCode = fail(".ell file magic is not RNAM", 60);
        lib->deleteEntry(ComponentType::TradingApp, kTestName);
        delete lib;
        return 0;
    }
    std::fprintf(stdout, "    magic = RNAM (OK)\n");

    // --- Step 8: Cleanup — deleteEntry ---
    std::fprintf(stdout, "\n[8] deleteEntry(TradingApp, \"%s\")...\n",
                 kTestName);
    bool deleted = lib->deleteEntry(ComponentType::TradingApp, kTestName);
    std::fprintf(stdout, "    deleteEntry = %s\n", deleted ? "true" : "false");
    if (!deleted) {
        st->exitCode = fail("deleteEntry returned false", 70);
        // Still try to clean the .ell file manually.
        DeleteFileA(ellPath.c_str());
        delete lib;
        return 0;
    }

    // Verify the .ell file is gone after deletion.
    if (lib->ellFileExists(ComponentType::TradingApp, progId)) {
        std::fprintf(stderr, "    WARNING: .ell still exists after delete\n");
        DeleteFileA(ellPath.c_str());
    }

    // --- Step 9: SUCCESS ---
    std::fprintf(stdout,
                 "\nSUCCESS: TradingApp \"%s\" inserted (ProgId=%d), "
                 "compiled, .ell verified (RNAM), and cleaned up.\n",
                 kTestName, progId);

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_insert_tradingapp: TradingApp INSERT flow ===\n");
    std::fprintf(stdout, "TradingApp name: %s\n", kTestName);

    // Run the entire test on a worker thread with a 64 MB stack.
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
                 "\n=== test_insert_tradingapp complete (exit %d) ===\n",
                 st.exitCode);
    return st.exitCode;
}
