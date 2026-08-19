// test_get_source.cpp
//
// Integration test (RED-GREEN) for the library-level getSource() primitive that
// the future `get-source` CLI subcommand will wrap. getSource() retrieves the
// EasyLanguage source text of an existing warehouse item by name + component
// type.
//
// TVS-F.4 subtask 1 (RED step): this test exercises the library-level
// source-retrieval primitive that the future `get-source` CLI subcommand will
// wrap. getSource() does NOT exist yet in tsdev_lib.h / tsdev_lib.cpp, so this
// test FAILS TO COMPILE (RED) until the method is implemented. Once
// implemented, the library-level assertions should PASS (GREEN). The CLI-level
// E2E test (tests/test_get_source.ps1) is the other half of the TDD scaffold.
//
// The test is self-cleaning: it inserts probe entries (a strategy and a
// function) with KNOWN source text, calls getSource() to retrieve the source,
// asserts the returned string matches (or contains key substrings of) the
// inserted source, then deletes the probes.
//
// Flow:
//   1. Construct TsDevLib (loads orlib20.dll, patches, inits, EDIT control)
//   2. Pre-clean: findExistPos(strategyName, Strategy) — delete if exists
//   3. Pre-clean: findExistPos(funcName, Function) — delete if exists
//   4. createEntry(Strategy, strategyName, errCode) — assert ProgId > 0
//   5. saveSource(Strategy, progId, strategySource) — assert success
//   6. verifyModule(Strategy, progId) — assert no errors
//   7. getSource(Strategy, strategyName) — ASSERT returned string contains
//      key substrings from the inserted source (G1 happy path)
//   8. getSource(Strategy, "NonExistentName_GetSrcTest") — ASSERT returns
//      empty string (G2 nonexistent)
//   9. createEntry(Function, funcName, errCode) — assert ProgId > 0
//  10. saveSource(Function, progId, funcSource) — assert success
//  11. verifyModule(Function, progId) — assert no errors
//  12. getSource(Function, funcName) — ASSERT returned string contains
//      key substrings from the inserted source (G3 function type)
//  13. deleteEntry(Strategy, strategyName) — cleanup
//  14. deleteEntry(Function, funcName) — cleanup
//  15. closeWarehouse() — best-effort flush
//  16. Print PASS / FAIL
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
const char* kStrategyName = "zzz_test_get_source_strategy";

// Function probe name (function name must match the EL function name).
const char* kFuncName = "zzz_test_get_source_func";

// A name that is guaranteed not to exist in the warehouse.
const char* kNonExistentName = "NonExistentName_GetSrcTest";

// Minimal valid EasyLanguage strategy source. In EasyLanguage `{ ... }` is a
// multi-line COMMENT block, so the actual code must be OUTSIDE it. The leading
// comment names the strategy (TSDev convention); the body issues a simple
// market buy order. This mirrors the proven source from test_exists.cpp.
const char* kStrategySource =
    "{ zzz_test_get_source_strategy }\r\n"
    "Buy (\"EnterLong\") next bar at Market;\r\n";

// Minimal valid EasyLanguage function source. The function name must match
// the component name. This mirrors the pattern from test_upsert.cpp U5.
const char* kFuncSource =
    "zzz_test_get_source_func = 1;\r\n";

// Key substrings that MUST appear in the source returned by getSource().
// The warehouse may normalize whitespace/line endings, so we use substring
// checks rather than exact string comparison.
const char* kStrategyKeySubstr = "Buy";
const char* kStrategyKeySubstr2 = "EnterLong";
const char* kFuncKeySubstr = "zzz_test_get_source_func";

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

    // --- Step 2: Pre-clean strategy — ensure it does not already exist ---
    std::fprintf(stdout,
                 "\n[2] Pre-clean: findExistPos(\"%s\", Strategy)...\n",
                 kStrategyName);
    int existingStrat = lib->findExistPos(kStrategyName,
                                          ComponentType::Strategy);
    std::fprintf(stdout, "    findExistPos = %d\n", existingStrat);
    if (existingStrat > 0) {
        std::fprintf(stdout,
                     "    Entry already exists (ProgId=%d) — deleting...\n",
                     existingStrat);
        bool deleted = lib->deleteEntry(ComponentType::Strategy,
                                        kStrategyName);
        std::fprintf(stdout, "    deleteEntry = %s\n",
                     deleted ? "true" : "false");
    }

    // --- Step 3: Pre-clean function — ensure it does not already exist ---
    std::fprintf(stdout,
                 "\n[3] Pre-clean: findExistPos(\"%s\", Function)...\n",
                 kFuncName);
    int existingFunc = lib->findExistPos(kFuncName,
                                         ComponentType::Function);
    std::fprintf(stdout, "    findExistPos = %d\n", existingFunc);
    if (existingFunc > 0) {
        std::fprintf(stdout,
                     "    Entry already exists (ProgId=%d) — deleting...\n",
                     existingFunc);
        bool deleted = lib->deleteEntry(ComponentType::Function, kFuncName);
        std::fprintf(stdout, "    deleteEntry = %s\n",
                     deleted ? "true" : "false");
    }

    // --- Step 4: createEntry (strategy) ---
    std::fprintf(stdout, "\n[4] createEntry(Strategy, \"%s\")...\n",
                 kStrategyName);
    int errCode = 0;
    int progId = lib->createEntry(ComponentType::Strategy, kStrategyName,
                                  errCode);
    std::fprintf(stdout, "    createEntry = %d, errCode = 0x%X\n",
                 progId, errCode);
    if (progId <= 0) {
        st->exitCode = fail("createEntry returned ProgId <= 0", 20);
        delete lib;
        return 0;
    }

    // --- Step 5: saveSource (strategy) ---
    std::fprintf(stdout, "\n[5] saveSource(Strategy, %d, source)...\n",
                 progId);
    bool saved = lib->saveSource(ComponentType::Strategy, progId,
                                 kStrategySource);
    std::fprintf(stdout, "    saveSource = %s\n", saved ? "true" : "false");
    if (!saved) {
        st->exitCode = fail("saveSource returned false", 30);
        lib->deleteEntry(ComponentType::Strategy, kStrategyName);
        delete lib;
        return 0;
    }

    // --- Step 6: verifyModule (strategy — compile + persist) ---
    std::fprintf(stdout, "\n[6] verifyModule(Strategy, %d)...\n", progId);
    std::string errors =
        lib->verifyModule(ComponentType::Strategy, progId);
    if (errors.empty()) {
        std::fprintf(stdout, "    verifyModule: no errors (compilation OK)\n");
    } else {
        std::fprintf(stderr, "    verifyModule errors:\n%s\n",
                     errors.c_str());
        st->exitCode = fail("verifyModule returned compilation errors", 40);
        lib->deleteEntry(ComponentType::Strategy, kStrategyName);
        delete lib;
        return 0;
    }

    // --- Step 7: getSource for the EXISTING strategy (G1 happy path) ---
    // ASSERT the returned string contains key substrings from the inserted
    // source. The warehouse may normalize whitespace/line endings, so we use
    // substring checks (std::string::find) rather than exact equality.
    std::fprintf(stdout,
                 "\n[7] getSource(Strategy, \"%s\") — "
                 "asserting source contains key substrings...\n",
                 kStrategyName);
    std::string retrieved = lib->getSource(ComponentType::Strategy,
                                           kStrategyName);
    std::fprintf(stdout, "    getSource returned %zu bytes\n",
                 retrieved.size());
    if (retrieved.empty()) {
        st->exitCode = fail(
            "getSource returned empty string for an existing item", 50);
        lib->deleteEntry(ComponentType::Strategy, kStrategyName);
        delete lib;
        return 0;
    }
    // Check key substrings.
    if (retrieved.find(kStrategyKeySubstr) == std::string::npos) {
        std::fprintf(stderr,
                     "    FAIL: source does not contain \"%s\"\n",
                     kStrategyKeySubstr);
        std::fprintf(stderr, "    Retrieved source:\n%s\n",
                     retrieved.c_str());
        st->exitCode = fail(
            "getSource source missing key substring 'Buy'", 55);
        lib->deleteEntry(ComponentType::Strategy, kStrategyName);
        delete lib;
        return 0;
    }
    if (retrieved.find(kStrategyKeySubstr2) == std::string::npos) {
        std::fprintf(stderr,
                     "    FAIL: source does not contain \"%s\"\n",
                     kStrategyKeySubstr2);
        std::fprintf(stderr, "    Retrieved source:\n%s\n",
                     retrieved.c_str());
        st->exitCode = fail(
            "getSource source missing key substring 'EnterLong'", 56);
        lib->deleteEntry(ComponentType::Strategy, kStrategyName);
        delete lib;
        return 0;
    }
    std::fprintf(stdout,
                 "    PASS: getSource returned source containing \"%s\" "
                 "and \"%s\"\n",
                 kStrategyKeySubstr, kStrategyKeySubstr2);

    // --- Step 8: getSource for a NONEXISTENT name (G2) ---
    // ASSERT returns empty string (not a crash).
    std::fprintf(stdout,
                 "\n[8] getSource(Strategy, \"%s\") — "
                 "asserting returns empty string...\n",
                 kNonExistentName);
    std::string notFoundSrc = lib->getSource(ComponentType::Strategy,
                                             kNonExistentName);
    std::fprintf(stdout, "    getSource returned %zu bytes\n",
                 notFoundSrc.size());
    if (!notFoundSrc.empty()) {
        std::fprintf(stderr,
                     "    FAIL: getSource returned non-empty for nonexistent "
                     "item (size=%zu)\n",
                     notFoundSrc.size());
        st->exitCode = fail(
            "getSource returned non-empty string for a nonexistent item", 60);
        // Still clean up the probe entries below.
    } else {
        std::fprintf(stdout,
                     "    PASS: nonexistent item returned empty string\n");
    }

    // --- Step 9: createEntry (function) ---
    std::fprintf(stdout, "\n[9] createEntry(Function, \"%s\")...\n",
                 kFuncName);
    int funcErrCode = 0;
    int funcProgId = lib->createEntry(ComponentType::Function, kFuncName,
                                      funcErrCode);
    std::fprintf(stdout, "    createEntry = %d, errCode = 0x%X\n",
                 funcProgId, funcErrCode);
    if (funcProgId <= 0) {
        st->exitCode = fail("createEntry returned ProgId <= 0 for function",
                            70);
        lib->deleteEntry(ComponentType::Strategy, kStrategyName);
        delete lib;
        return 0;
    }

    // --- Step 10: saveSource (function) ---
    std::fprintf(stdout, "\n[10] saveSource(Function, %d, source)...\n",
                 funcProgId);
    bool funcSaved = lib->saveSource(ComponentType::Function, funcProgId,
                                     kFuncSource);
    std::fprintf(stdout, "    saveSource = %s\n",
                 funcSaved ? "true" : "false");
    if (!funcSaved) {
        st->exitCode = fail("saveSource returned false for function", 80);
        lib->deleteEntry(ComponentType::Function, kFuncName);
        lib->deleteEntry(ComponentType::Strategy, kStrategyName);
        delete lib;
        return 0;
    }

    // --- Step 11: verifyModule (function — compile + persist) ---
    std::fprintf(stdout, "\n[11] verifyModule(Function, %d)...\n",
                 funcProgId);
    std::string funcErrors =
        lib->verifyModule(ComponentType::Function, funcProgId);
    if (funcErrors.empty()) {
        std::fprintf(stdout,
                     "    verifyModule: no errors (compilation OK)\n");
    } else {
        std::fprintf(stderr, "    verifyModule errors:\n%s\n",
                     funcErrors.c_str());
        st->exitCode = fail(
            "verifyModule returned compilation errors for function", 90);
        lib->deleteEntry(ComponentType::Function, kFuncName);
        lib->deleteEntry(ComponentType::Strategy, kStrategyName);
        delete lib;
        return 0;
    }

    // --- Step 12: getSource for the EXISTING function (G3 function type) ---
    // ASSERT the returned string contains key substrings from the inserted
    // function source.
    std::fprintf(stdout,
                 "\n[12] getSource(Function, \"%s\") — "
                 "asserting source contains key substrings...\n",
                 kFuncName);
    std::string funcRetrieved = lib->getSource(ComponentType::Function,
                                               kFuncName);
    std::fprintf(stdout, "    getSource returned %zu bytes\n",
                 funcRetrieved.size());
    if (funcRetrieved.empty()) {
        st->exitCode = fail(
            "getSource returned empty string for an existing function", 100);
        lib->deleteEntry(ComponentType::Function, kFuncName);
        lib->deleteEntry(ComponentType::Strategy, kStrategyName);
        delete lib;
        return 0;
    }
    if (funcRetrieved.find(kFuncKeySubstr) == std::string::npos) {
        std::fprintf(stderr,
                     "    FAIL: function source does not contain \"%s\"\n",
                     kFuncKeySubstr);
        std::fprintf(stderr, "    Retrieved source:\n%s\n",
                     funcRetrieved.c_str());
        st->exitCode = fail(
            "getSource function source missing key substring", 105);
        lib->deleteEntry(ComponentType::Function, kFuncName);
        lib->deleteEntry(ComponentType::Strategy, kStrategyName);
        delete lib;
        return 0;
    }
    std::fprintf(stdout,
                 "    PASS: getSource returned function source containing "
                 "\"%s\"\n",
                 kFuncKeySubstr);

    // --- Step 13: Cleanup — delete the strategy probe ---
    std::fprintf(stdout,
                 "\n[13] Cleanup: deleteEntry(Strategy, \"%s\")...\n",
                 kStrategyName);
    bool stratDeleted = lib->deleteEntry(ComponentType::Strategy,
                                         kStrategyName);
    std::fprintf(stdout, "    deleteEntry = %s\n",
                 stratDeleted ? "true" : "false");
    if (!stratDeleted) {
        st->exitCode = fail("cleanup deleteEntry failed for strategy", 110);
    }

    // --- Step 14: Cleanup — delete the function probe ---
    std::fprintf(stdout,
                 "\n[14] Cleanup: deleteEntry(Function, \"%s\")...\n",
                 kFuncName);
    bool funcDeleted = lib->deleteEntry(ComponentType::Function, kFuncName);
    std::fprintf(stdout, "    deleteEntry = %s\n",
                 funcDeleted ? "true" : "false");
    if (!funcDeleted) {
        st->exitCode = fail("cleanup deleteEntry failed for function", 120);
    }

    // --- Step 15: closeWarehouse (best-effort flush) ---
    std::fprintf(stdout, "\n[15] closeWarehouse()...\n");
    lib->closeWarehouse();
    std::fprintf(stdout, "    closeWarehouse done\n");

    // --- Step 16: Result ---
    if (st->exitCode == 0) {
        std::fprintf(stdout,
                     "\nPASS: getSource correctly retrieves source for "
                     "strategy and function, returns empty for nonexistent\n");
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
                 "=== test_get_source: getSource retrieval (TVS-F.4) ===\n");
    std::fprintf(stdout, "Strategy name: %s\n", kStrategyName);
    std::fprintf(stdout, "Function name: %s\n", kFuncName);
    std::fprintf(stdout, "Nonexistent name: %s\n", kNonExistentName);

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

    std::fprintf(stdout, "\n=== test_get_source complete (exit %d) ===\n",
                 st.exitCode);
    return st.exitCode;
}
