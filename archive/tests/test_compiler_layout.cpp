// test_compiler_layout.cpp
//
// Workstream M slice M.1 — TDD RED->GREEN integration test for the
// CELCompiler allocation / setup / destruction parity with TSDev.exe.
//
// Historical M.1 facts (Ghidra static + Frida trace cross-check; see the
// tracked `docs/M1_HANDOFF.md` §§R-M.1.1–R-M.1.5). The underlying tmp/ notes are
// transient and unavailable in a fresh tracked checkout:
//
//   R-M.1.1 — TSDev allocates+constructs the CELCompiler via orlib20 ordinal
//   export 66 (FUN_10165f70 = operator_new(0x58) + ctor) and destructs+frees
//   via ordinal 67 (FUN_10165fd0 = dtor + operator delete(p, 0x58)).
//
//   R-M.1.2 — Field layout written by TSDev (FUN_00451800 at
//   0x00451ac4-0x00451af9):
//     +0x08 = category (whType low 8 bits)   — getCategory(type)
//     +0x0C = subtype                        — getSubType(type)
//     +0x10 = ProgId                         — the progId arg
//     +0x18 = notification HWND              — m_editorHwnd
//     +0x00, +0x04 = result fields, zeroed by FUN_004D9480 AFTER
//                    StartCompilingProcess, BEFORE _VerifyModule_New.
//     +0x14 = ctor-zeroed (not written by verify chain).
//
//   R-M.1.3 — +0x28 is NOT WRITTEN by TSDev. The CLI's prior +0x28 = 6 write
//   has no TSDev counterpart and must be removed (left as uninitialized heap;
//   ctor does NOT zero it — runtime value is indeterminate garbage).
//
//   R-M.1.4 — +0x18 = notification HWND (same handle passed to
//   _VerifyModule_New as hwnd arg; Frida trace confirms
//   _SaveModule_New editorHwnd == _VerifyModule_New hwnd).
//
//   R-M.1.5 — Category/subtype come from getCategory(type) / getSubType(type)
//   (warehouse type mapping), NOT from source-text parsing.
//
// This test exercises the onCompilerReady test hook (populated via
// makeTsDevLibForTest) which verifyModule() invokes immediately BEFORE the
// _VerifyModule_New call (after StartCompilingProcess and after the
// +0x00/+0x04 result-field pre-zeroing). The callback captures the live
// compiler object pointer, component type, and progId into thread_local
// globals, then the test reads the 0x58-byte object at the relevant offsets
// and asserts the TSDev-parity layout.
//
// Two test cases:
//   1. Strategy (category=0, subtype=0) — the historical pre-M.1 case caught
//      +0x10 (ProgId vs 0xA02) and +0x28 (0 vs 6) divergences.
//   2. Indicator (category=2, subtype=0) — the historical pre-M.1 case also
//      caught the +0x08 divergence (Strategy has category=0, so +0x08=0 could
//      pass even when the field was omitted). The current M.1 implementation
//      writes the TSDev-parity fields for both cases.
//
// Both cases insert a disposable Strategy/Indicator item, call verifyModule
// (which fires the hook), assert the layout, then delete the disposable item.
//
// REQUIRES TradeStation: this is an integration test that constructs a real
// TsDevLib (loads orlib20.dll, applies patches, calls OrlibInit, creates the
// hidden EDIT control) and inserts + verifies + deletes real warehouse
// entries. This is a library-level test; Desktop/TSDev running state is not a
// prerequisite because the test does not exercise the Desktop UI.
//
// Runs on a 64 MB-stack worker thread (EasyLanguage compiler needs far more
// than the default 1 MB, and the hidden EDIT control is thread-affine so
// TsDevLib must live on this thread).
//
// Exit code: 0 only if ALL assertions pass; nonzero on any failure.

#include "../src/tsdev_lib_test_hooks.h"  // internal/test-only header (NOT public)
#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <windows.h>

using namespace tsdev;

namespace {

// ---------------------------------------------------------------------------
// CHECK macro — prints PASS/FAIL per assertion with line numbers.
// ---------------------------------------------------------------------------
static int g_failCount = 0;
static int g_passCount = 0;

#define CHECK(cond, msg) do {                                      \
    if (!(cond)) {                                                 \
        std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        g_failCount++;                                             \
    } else {                                                       \
        std::printf("PASS: %s\n", msg);                            \
        g_passCount++;                                             \
    }                                                              \
} while(0)

// ---------------------------------------------------------------------------
// Thread-local capture state for the onCompilerReady hook.
//
// The hook runs on the worker thread (the same thread verifyModule runs on)
// and writes into these globals. The test reads them after verifyModule
// returns. thread_local ensures each worker thread has its own copy (the
// main thread's copy stays zeroed).
// ---------------------------------------------------------------------------
struct CompilerCapture {
    void* compilerMem    = nullptr;
    int   componentType  = -1;
    int   progId         = -1;
    bool  fired          = false;
};

thread_local CompilerCapture g_capture;

void onCompilerReadyHook(void* compilerMem, int componentType, int progId) {
    g_capture.compilerMem   = compilerMem;
    g_capture.componentType = componentType;
    g_capture.progId        = progId;
    g_capture.fired         = true;
}

// ---------------------------------------------------------------------------
// Real ComApi — binds the actual CoInitializeEx / CoUninitialize so the
// test-only ctor enters a real STA apartment (required for orlib20 / OLE).
// ---------------------------------------------------------------------------
ComApi realComApi() {
    return ComApi{&CoInitializeEx, &CoUninitialize};
}

// Build a TsDevLibTestConfig with the real COM API and the onCompilerReady
// hook installed. throwAfterCom is false so the ctor proceeds with the full
// init sequence (loads orlib20, applies patches, OrlibInit, EDIT control).
TsDevLibTestConfig makeConfigWithHook() {
    TsDevLibTestConfig cfg;
    cfg.comApi          = realComApi();
    cfg.throwAfterCom   = false;
    cfg.onCompilerReady = &onCompilerReadyHook;
    return cfg;
}

// ---------------------------------------------------------------------------
// Test fixture: per-case state.
// ---------------------------------------------------------------------------
struct TestCase {
    ComponentType type;
    const char*   name;
    const char*   source;
    int           expectedCategory;  // getCategory(type)
    int           expectedSubType;   // getSubType(type)
};

// Minimal valid EasyLanguage source for each component type.
// Strategy: a simple market buy order.
// Indicator: a simple Plot1 of the close.
const char* kStrategySource =
    "{ TS_AUTO_M1_TEST_STRATEGY }\r\n"
    "Buy (\"EnterLong\") next bar at Market;\r\n";

const char* kIndicatorSource =
    "{ TS_AUTO_M1_TEST_INDICATOR }\r\n"
    "Plot1(Close, \"Close\");\r\n";

// Delete the .ell file for a given ProgId so the test is repeatable.
bool cleanEllFile(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        return true;  // already absent
    }
    return (DeleteFileA(path.c_str()) != 0);
}

// Print a failure banner and return the given code.
int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// ---------------------------------------------------------------------------
// Run one test case against an ALREADY-CONSTRUCTED TsDevLib (passed by
// reference). The caller (runTest) constructs ONE TsDevLib for the whole
// worker thread and passes it to both cases — this matches the operational
// invariant used by every other integration test in this codebase (see
// test_auto_cleanup.cpp, test_stress_batch.cpp): a SINGLE TsDevLib per
// worker thread. Constructing a second TsDevLib on the same thread (the
// previous structure here) leaves orlib20 global state in a bad state after
// the first instance's destructor runs OrlibExit and the second instance's
// constructor runs OrlibInit again, causing _VerifyModule_New to raise an
// SEH exception on the second case.
//
// The onCompilerReady hook is a function pointer that writes to thread_local
// globals, so it works correctly with a single shared instance across both
// cases (the capture is reset at the top of each case).
//
// Returns 0 on success, nonzero on failure. Updates g_failCount/g_passCount.
// ---------------------------------------------------------------------------
int runOneCase(TsDevLib& lib, const TestCase& tc, int caseNum) {
    std::printf("\n=== Case %d: %s (name=%s) ===\n",
                caseNum,
                (tc.type == ComponentType::Strategy ? "Strategy" : "Indicator"),
                tc.name);

    // Reset the capture so a stale value from a previous case cannot mask a
    // "hook did not fire" failure.
    g_capture = CompilerCapture{};

    // --- 1. TsDevLib already constructed by runTest (hook installed) ---
    std::printf("  [1] Using shared TsDevLib (test-only ctor, hook installed)\n");

    // --- 2. If the test item already exists, delete it first (re-run safe) ---
    int existingProgId = lib.findExistPos(tc.name, tc.type);
    if (existingProgId > 0) {
        std::printf("  [2] Pre-cleanup: deleting existing %s (ProgId=%d)\n",
                    tc.name, existingProgId);
        // Clean the .ell so the insert path is exercised fresh.
        std::string ellPath = lib.getEllPath(tc.type, existingProgId);
        cleanEllFile(ellPath);
        lib.deleteEntry(tc.type, tc.name, /*flush=*/false);
    }

    // --- 3. createEntry ---
    std::printf("  [3] createEntry(%s, \"%s\")...\n",
                (tc.type == ComponentType::Strategy ? "Strategy" : "Indicator"),
                tc.name);
    int errCode = 0;
    int progId = lib.createEntry(tc.type, tc.name, errCode);
    std::printf("      createEntry = %d, errCode = 0x%X\n", progId, errCode);
    if (progId <= 0) {
        return fail("createEntry returned ProgId <= 0", 100 + caseNum * 10 + 2);
    }

    // --- 4. saveSource ---
    std::printf("  [4] saveSource(%s, %d, source)...\n",
                (tc.type == ComponentType::Strategy ? "Strategy" : "Indicator"),
                progId);
    if (!lib.saveSource(tc.type, progId, tc.source)) {
        // Best-effort cleanup of the partial entry.
        lib.deleteEntry(tc.type, tc.name, /*flush=*/false);
        return fail("saveSource returned false", 100 + caseNum * 10 + 3);
    }

    // --- 5. verifyModule (fires the hook) ---
    std::printf("  [5] verifyModule(%s, %d)...\n",
                (tc.type == ComponentType::Strategy ? "Strategy" : "Indicator"),
                progId);
    std::string errors = lib.verifyModule(tc.type, progId);
    if (!errors.empty()) {
        std::fprintf(stderr, "      verifyModule errors:\n%s\n", errors.c_str());
        lib.deleteEntry(tc.type, tc.name, /*flush=*/false);
        return fail("verifyModule returned compilation errors", 100 + caseNum * 10 + 4);
    }
    std::printf("      verifyModule: no errors (compilation OK)\n");

    // --- 6. Assert the hook fired ---
    CHECK(g_capture.fired,
          "onCompilerReady hook fired during verifyModule");
    if (!g_capture.fired) {
        lib.deleteEntry(tc.type, tc.name, /*flush=*/false);
        return fail("hook did not fire — cannot assert layout", 100 + caseNum * 10 + 5);
    }

    // --- 7. Assert the captured component type and progId ---
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "captured componentType==%d (expected %d) [case %d]",
                      g_capture.componentType, static_cast<int>(tc.type), caseNum);
        CHECK(g_capture.componentType == static_cast<int>(tc.type), buf);
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "captured progId==%d (expected %d) [case %d]",
                      g_capture.progId, progId, caseNum);
        CHECK(g_capture.progId == progId, buf);
    }

    // --- 8. Assert the TSDev-parity field layout ---
    // Read the 0x58-byte object at the relevant offsets. The hook fired
    // AFTER StartCompilingProcess and AFTER the +0x00/+0x04 pre-zeroing, so
    // +0x00 and +0x04 must be 0 at this point (they are zeroed by
    // FUN_004D9480 before _VerifyModule_New).
    //
    // NOTE: the compiler object is still LIVE here (verifyModule has not
    // returned yet — the hook runs synchronously inside it). The object
    // remains valid until callDestroy runs after _VerifyModule_New returns.
    // Reading the fields here is safe.
    char* base = static_cast<char*>(g_capture.compilerMem);
    if (!base) {
        lib.deleteEntry(tc.type, tc.name, /*flush=*/false);
        return fail("captured compilerMem is null", 100 + caseNum * 10 + 6);
    }

    int   f00 = *reinterpret_cast<int*>(base + 0x00);
    int   f04 = *reinterpret_cast<int*>(base + 0x04);
    int   f08 = *reinterpret_cast<int*>(base + 0x08);
    int   f0C = *reinterpret_cast<int*>(base + 0x0C);
    int   f10 = *reinterpret_cast<int*>(base + 0x10);
    HWND  f18 = *reinterpret_cast<HWND*>(base + 0x18);
    // +0x28 is intentionally NOT read or asserted here. Per the historical M.1
    // handoff (`docs/M1_HANDOFF.md` §R-M.1.3; raw note unavailable), +0x28 is
    // NOT written by TSDev's
    // CELCompiler constructor nor by operator_new inside ordinal 66, so it
    // "remains uninitialized" — its runtime value is indeterminate garbage
    // (observed as 0 in one run and 71704709 in another). The M.1 parity for
    // +0x28 is that OUR code does NOT WRITE to it (the legacy `+0x28 = 6`
    // write was removed); that parity is verified by code review, NOT by
    // reading the runtime value. Asserting `+0x28 == 0` was invalid and has
    // been removed.

    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "+0x00==0 (result field, pre-zeroed) [case %d, got 0x%X]",
                      caseNum, static_cast<unsigned>(f00));
        CHECK(f00 == 0, buf);
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "+0x04==0 (cumulative result field, pre-zeroed) [case %d, got 0x%X)",
                      caseNum, static_cast<unsigned>(f04));
        CHECK(f04 == 0, buf);
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "+0x08==%d (category) [case %d, got %d]",
                      tc.expectedCategory, caseNum, f08);
        CHECK(f08 == tc.expectedCategory, buf);
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "+0x0C==%d (subtype) [case %d, got %d]",
                      tc.expectedSubType, caseNum, f0C);
        CHECK(f0C == tc.expectedSubType, buf);
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "+0x10==%d (ProgId) [case %d, got 0x%X]",
                      progId, caseNum, static_cast<unsigned>(f10));
        CHECK(f10 == progId, buf);
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "+0x18!=nullptr (HWND non-NULL) [case %d, got %p]",
                      caseNum, static_cast<void*>(f18));
        CHECK(f18 != nullptr, buf);
    }

    // --- 9. Cleanup: delete the disposable item ---
    std::printf("  [9] Cleanup: deleteEntry(%s, \"%s\")...\n",
                (tc.type == ComponentType::Strategy ? "Strategy" : "Indicator"),
                tc.name);
    if (!lib.deleteEntry(tc.type, tc.name, /*flush=*/true)) {
        std::fprintf(stderr,
                     "  WARNING: deleteEntry returned false (residual artifact?)\n");
    }

    // Reset capture so it does not leak into the next case.
    g_capture = CompilerCapture{};

    return 0;
}

// ---------------------------------------------------------------------------
// Worker thread entry point. Constructs ONE TsDevLib (with the onCompilerReady
// hook installed) for the whole worker thread and runs both test cases against
// it. This matches the operational invariant used by every other integration
// test in this codebase (test_auto_cleanup.cpp, test_stress_batch.cpp): a
// SINGLE TsDevLib instance per worker thread. Constructing a second TsDevLib
// on the same thread leaves orlib20 global state in a bad state (the first
// instance's destructor runs OrlibExit, then the second instance's
// constructor runs OrlibInit again), causing _VerifyModule_New to raise an
// SEH exception on the second case.
//
// The 64 MB stack is required by the EasyLanguage compiler invoked via
// VerifyModule_New.
// ---------------------------------------------------------------------------
struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);

    // --- Construct the single shared TsDevLib for this worker thread ---
    // The onCompilerReady hook is installed via the test-only factory +
    // TsDevLibTestConfig. The hook is a function pointer that writes to
    // thread_local globals, so it works fine with a single shared instance
    // across both cases (the capture is reset at the top of each case).
    std::printf("\n--- Constructing shared TsDevLib (test-only ctor, hook installed) ---\n");
    std::unique_ptr<TsDevLib> lib;
    try {
        TsDevLibTestConfig cfg = makeConfigWithHook();
        lib = tsdev::makeTsDevLibForTest(/*tsPathOverride=*/nullptr, cfg);
    } catch (const std::exception& e) {
        st->exitCode = fail(
            (std::string("TsDevLib construction threw: ") + e.what()).c_str(),
            100);
        return 0;
    }
    if (!lib) {
        st->exitCode = fail("makeTsDevLibForTest returned null", 101);
        return 0;
    }
    std::printf("--- TsDevLib constructed OK ---\n");

    // Use a date-stamped name so the test items are unique per run day and
    // do not collide with any pre-existing entries. The format
    // TS_AUTO_M1_TEST_YYYYMMDD matches the convention in the task spec.
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    char stratName[64];
    char indName[64];
    std::snprintf(stratName, sizeof(stratName),
                  "TS_AUTO_M1_TEST_STRAT_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(indName, sizeof(indName),
                  "TS_AUTO_M1_TEST_IND_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);

    TestCase case1{
        ComponentType::Strategy,
        stratName,
        kStrategySource,
        getCategory(ComponentType::Strategy),  // 0
        getSubType(ComponentType::Strategy)    // 0
    };
    TestCase case2{
        ComponentType::Indicator,
        indName,
        kIndicatorSource,
        getCategory(ComponentType::Indicator),  // 2
        getSubType(ComponentType::Indicator)    // 0
    };

    int rc1 = runOneCase(*lib, case1, 1);
    if (rc1 != 0) {
        st->exitCode = rc1;
        return 0;
    }

    int rc2 = runOneCase(*lib, case2, 2);
    if (rc2 != 0) {
        st->exitCode = rc2;
        return 0;
    }

    st->exitCode = (g_failCount == 0) ? 0 : 1;
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== test_compiler_layout: M.1 CELCompiler field-layout parity ===\n");
    std::printf("Asserts the 0x58-byte CELCompiler object written by verifyModule()\n");
    std::printf("matches the TSDev.exe field layout (Ghidra + Frida confirmed).\n");
    std::printf("Desktop/TSDev running state is not a prerequisite for this library-level test.\n");

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

    std::printf("\n=== test_compiler_layout: %d/%d assertions passed, %d failed ===\n",
                g_passCount, g_passCount + g_failCount, g_failCount);
    std::printf("=== test_compiler_layout complete (exit %d) ===\n",
                st.exitCode);
    return st.exitCode;
}
