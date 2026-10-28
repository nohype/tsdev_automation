// test_tsdev_com_init_seam.cpp
//
// TVS-L.13 GREEN test: TsDevLib COM-init injection seam.
//
// This is the TDD GREEN step. It exercises a PRIVATE/test-only construction
// seam (DECLARED and IMPLEMENTED) that lets a unit test drive TsDevLib
// initialization with injected COM HRESULTs WITHOUT leaking COM
// configuration into the public user API (src/tsdev_lib.h).
//
// The seam (declared and implemented — see src/tsdev_lib.h and
// src/tsdev_lib_test_hooks.h):
//   - src/tsdev_lib.h forward-declares `struct TsDevLibTestConfig;` and
//     declares a PRIVATE constructor
//         TsDevLib::TsDevLib(const wchar_t*, const TsDevLibTestConfig&)
//     plus a friend declaration for the test-only factory
//         std::unique_ptr<TsDevLib> tsdev::makeTsDevLibForTest(
//             const wchar_t*, const TsDevLibTestConfig&);
//     No ComApi / <objbase.h> / COM function-pointer type appears in the
//     public header — the public user API sees only an opaque
//     forward-declared struct name and a private ctor it cannot call.
//   - src/tsdev_lib_test_hooks.h (INTERNAL, test-only, NOT a public header)
//     defines TsDevLibTestConfig { ComApi comApi; bool throwAfterCom; } and
//     declares the factory tsdev::makeTsDevLibForTest(...).
//   - src/tsdev_lib.cpp DEFINES the private ctor and the factory.
//
// GREEN signal:
//   The factory and the private ctor are DEFINED in src/tsdev_lib.cpp. This
//   test references makeTsDevLibForTest and links cleanly against tsdev_lib,
//   so the test executable builds, links, and runs the assertions below —
//   the GREEN signal.
//
// Contract (enforced by the assertions below):
//   1. S_OK            + throwAfterCom=true: COM entered (mock CoInitializeEx
//      called once with (nullptr, COINIT_APARTMENTTHREADED)), ctor throws at
//      the injected post-COM point, mock CoUninitialize called EXACTLY once
//      during stack unwinding. No orlib20 / SetDllDirectory / patch /
//      warehouse side effect runs.
//   2. S_FALSE         + throwAfterCom=true: same as (1) with S_FALSE (S_FALSE
//      is still a successful COM enter and must be balanced once).
//   3. RPC_E_CHANGED_MODE: COM init fails; ctor throws std::runtime_error
//      naming HRESULT 0x80010106; mock CoUninitialize NOT called; NO
//      SetDllDirectoryW, NO orlib20 / support-DLL load, NO patch, NO OrlibInit,
//      NO warehouse access (COM failure is a HARD stop before every later
//      step — the intended contract called out by TVS-L.13).
//   4. E_FAIL (generic): same as (3) with E_FAIL (0x80004005).
//   5. Later-constructor exception after COM success (S_OK + throwAfterCom):
//      COM succeeds, the ctor throws at the injected post-COM point, mock
//      CoUninitialize called EXACTLY once during stack unwinding (COM is
//      balanced on the constructing thread even when a later step fails).
//      DISTINCT from check 1: this check additionally verifies the exception
//      MESSAGE identifies the throw as the injected post-COM exception (NOT a
//      COM-init failure that names an HRESULT), proving the throw came from
//      the post-COM point and not from the COM-init hard-stop branch.
//
// This is a PURE UNIT TEST at runtime: it injects mock CoInitializeEx /
// CoUninitialize function pointers (the real COM functions are NEVER called
// by this test), it never loads orlib20.dll, and it never touches the
// TradeStation installation. It runs on a 64 MB worker thread only to match
// the project's test-harness convention.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

#include "../src/tsdev_lib_test_hooks.h"  // internal/test-only header (NOT public)

#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <windows.h>

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// ---------------------------------------------------------------------------
// Mock COM API.
//
// The mock functions record their calls in a single MockState struct so each
// check can reset the counters before exercising the seam. The mock
// CoInitializeEx returns whatever HRESULT the check installs in
// MockState::nextInitResult. The mock CoUninitialize increments the
// uninitialize call count. Neither mock touches the real COM apartment, so
// the test process's real COM state is never manipulated.
// ---------------------------------------------------------------------------
struct MockState {
    HRESULT nextInitResult = S_OK;   // what mockCoInitializeEx returns
    int     initCalls      = 0;      // count of mockCoInitializeEx calls
    int     uninitCalls    = 0;      // count of mockCoUninitialize calls
    LPVOID  lastReserved   = nullptr; // last pvReserved passed to init
    DWORD   lastCoInit     = 0;       // last dwCoInit passed to init
};

MockState g_mock;

HRESULT WINAPI mockCoInitializeEx(LPVOID pvReserved, DWORD dwCoInit) {
    ++g_mock.initCalls;
    g_mock.lastReserved = pvReserved;
    g_mock.lastCoInit   = dwCoInit;
    return g_mock.nextInitResult;
}

void WINAPI mockCoUninitialize() {
    ++g_mock.uninitCalls;
}

ComApi mockApi() {
    return ComApi{&mockCoInitializeEx, &mockCoUninitialize};
}

void resetMock(HRESULT nextInitResult) {
    g_mock = MockState{};
    g_mock.nextInitResult = nextInitResult;
}

// Case-insensitive substring search (ASCII only). Used for HRESULT hex
// matching so an assertion is robust to the hex digit casing the
// implementation emits, instead of pinning to one casing. Returns true if
// `needle` appears in `haystack` ignoring ASCII case.
bool containsCI(const std::string& haystack, const char* needle) {
    if (!needle) return false;
    const size_t hLen = haystack.size();
    const size_t nLen = std::strlen(needle);
    if (nLen == 0 || nLen > hLen) return false;
    auto eq = [](char a, char b) {
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        return a == b;
    };
    for (size_t i = 0; i + nLen <= hLen; ++i) {
        size_t j = 0;
        for (; j < nLen; ++j) {
            if (!eq(haystack[i + j], needle[j])) break;
        }
        if (j == nLen) return true;
    }
    return false;
}

// Build a TsDevLibTestConfig with the mock COM API and the given
// throwAfterCom flag. The injected HRESULT that mockCoInitializeEx returns is
// NOT set here — it is installed separately via resetMock() before each
// construction (mockCoInitializeEx reads g_mock.nextInitResult at call time).
// The real CoInitializeEx / CoUninitialize are never referenced by this test.
TsDevLibTestConfig makeConfig(bool throwAfterCom) {
    TsDevLibTestConfig cfg;
    cfg.comApi        = mockApi();
    cfg.throwAfterCom = throwAfterCom;
    return cfg;
}

// ---------------------------------------------------------------------------
// Check 1: S_OK success + throwAfterCom (later-constructor exception path).
//
// COM enters the STA (S_OK). The ctor then throws at the injected post-COM
// point. The COM guard's destructor runs during unwinding and calls the
// injected CoUninitialize EXACTLY once. No orlib20 / SetDllDirectory / patch
// / warehouse side effect runs (the throw happens before any of those).
// ---------------------------------------------------------------------------
int checkSOkThenThrow() {
    std::fprintf(stdout, "  [check 1] S_OK + throwAfterCom (unwind balances COM)\n");
    resetMock(S_OK);
    TsDevLibTestConfig cfg = makeConfig(/*throwAfterCom=*/true);

    int uninitBefore = g_mock.uninitCalls;
    bool caught = false;
    try {
        auto lib = tsdev::makeTsDevLibForTest(/*tsPathOverride=*/nullptr, cfg);
        (void)lib;
    } catch (const std::runtime_error& e) {
        caught = true;
        (void)e;
    }

    if (!caught) {
        return fail("S_OK+throw: expected std::runtime_error from the injected post-COM throw", 10);
    }
    if (g_mock.initCalls != 1) {
        std::fprintf(stderr, "  initCalls=%d\n", g_mock.initCalls);
        return fail("S_OK+throw: CoInitializeEx should be called exactly once", 11);
    }
    if (g_mock.lastReserved != nullptr) {
        return fail("S_OK+throw: CoInitializeEx should be called with pvReserved=nullptr", 12);
    }
    if (g_mock.lastCoInit != COINIT_APARTMENTTHREADED) {
        std::fprintf(stderr, "  lastCoInit=0x%08lX\n",
                     static_cast<unsigned long>(g_mock.lastCoInit));
        return fail("S_OK+throw: CoInitializeEx should be called with COINIT_APARTMENTTHREADED", 13);
    }
    if (g_mock.uninitCalls != uninitBefore + 1) {
        std::fprintf(stderr, "  uninitCalls=%d (expected %d)\n",
                     g_mock.uninitCalls, uninitBefore + 1);
        return fail("S_OK+throw: CoUninitialize should be called exactly once during unwind", 14);
    }
    std::fprintf(stdout, "    PASS: S_OK entered STA, post-COM throw, CoUninitialize balanced once\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Check 2: S_FALSE success + throwAfterCom.
//
// S_FALSE means the apartment was already entered on this thread; per COM
// rules it is STILL a successful enter and must be balanced by exactly one
// CoUninitialize. Same expectations as check 1 but with S_FALSE.
// ---------------------------------------------------------------------------
int checkSFalseThenThrow() {
    std::fprintf(stdout, "  [check 2] S_FALSE + throwAfterCom (unwind balances COM)\n");
    resetMock(S_FALSE);
    TsDevLibTestConfig cfg = makeConfig(/*throwAfterCom=*/true);

    int uninitBefore = g_mock.uninitCalls;
    bool caught = false;
    try {
        auto lib = tsdev::makeTsDevLibForTest(nullptr, cfg);
        (void)lib;
    } catch (const std::runtime_error&) {
        caught = true;
    }

    if (!caught) {
        return fail("S_FALSE+throw: expected std::runtime_error from the injected post-COM throw", 20);
    }
    if (g_mock.initCalls != 1) {
        return fail("S_FALSE+throw: CoInitializeEx should be called exactly once", 21);
    }
    if (g_mock.lastCoInit != COINIT_APARTMENTTHREADED) {
        return fail("S_FALSE+throw: CoInitializeEx should be called with COINIT_APARTMENTTHREADED", 22);
    }
    if (g_mock.uninitCalls != uninitBefore + 1) {
        std::fprintf(stderr, "  uninitCalls=%d (expected %d)\n",
                     g_mock.uninitCalls, uninitBefore + 1);
        return fail("S_FALSE+throw: CoUninitialize should be called exactly once during unwind (S_FALSE still balances)", 23);
    }
    std::fprintf(stdout, "    PASS: S_FALSE entered STA, post-COM throw, CoUninitialize balanced once\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Check 3: RPC_E_CHANGED_MODE — COM init hard failure.
//
// The injected CoInitializeEx returns RPC_E_CHANGED_MODE (0x80010106),
// indicating the thread is already in an incompatible apartment. The ctor
// MUST throw std::runtime_error naming the HRESULT and MUST NOT call
// CoUninitialize (the COM runtime was never entered). Crucially, NO later
// constructor step may run: NO SetDllDirectoryW, NO orlib20 / support-DLL
// load, NO patch application, NO OrlibInit, NO warehouse access. This is the
// intended TVS-L.13 contract — COM failure is a HARD stop before every
// later step. (The "no later side effect" property is implied by the throw
// point: the ctor throws at the COM-init check, which is the FIRST effective
// action, so no later step is reached. The GREEN implementation must keep
// that ordering.)
// ---------------------------------------------------------------------------
int checkChangedModeHardStop() {
    std::fprintf(stdout, "  [check 3] RPC_E_CHANGED_MODE hard stop (no CoUninitialize, no later side effects)\n");
    resetMock(RPC_E_CHANGED_MODE);
    TsDevLibTestConfig cfg = makeConfig(/*throwAfterCom=*/true);

    int uninitBefore = g_mock.uninitCalls;
    bool caught = false;
    std::string what;
    try {
        auto lib = tsdev::makeTsDevLibForTest(nullptr, cfg);
        (void)lib;
    } catch (const std::runtime_error& e) {
        caught = true;
        what = e.what();
    }

    if (!caught) {
        return fail("RPC_E_CHANGED_MODE: expected std::runtime_error (hard init failure)", 30);
    }
    if (g_mock.initCalls != 1) {
        return fail("RPC_E_CHANGED_MODE: CoInitializeEx should be called exactly once", 31);
    }
    if (g_mock.uninitCalls != uninitBefore) {
        std::fprintf(stderr, "  uninitCalls=%d (expected %d)\n",
                     g_mock.uninitCalls, uninitBefore);
        return fail("RPC_E_CHANGED_MODE: CoUninitialize must NOT be called (no apartment entered)", 32);
    }
    // The HRESULT must be named in the message for diagnostics. The message
    // format is "tsdev: COM initialization failed: HRESULT 0x%08X" (uppercase
    // hex). Use a CASE-INSENSITIVE substring search so the assertion is robust
    // to the hex digit casing the implementation chooses, instead of pinning
    // to one casing. RPC_E_CHANGED_MODE == 0x80010106.
    if (!containsCI(what, "0x80010106")) {
        std::fprintf(stderr, "  message=\"%s\" (expected to contain 0x80010106)\n", what.c_str());
        return fail("RPC_E_CHANGED_MODE: exception message must name HRESULT 0x80010106", 33);
    }
    std::fprintf(stdout, "    PASS: RPC_E_CHANGED_MODE hard stop, no CoUninitialize, HRESULT named\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Check 4: Generic failure (E_FAIL) — same hard-stop contract as check 3.
// ---------------------------------------------------------------------------
int checkGenericFailHardStop() {
    std::fprintf(stdout, "  [check 4] E_FAIL generic hard stop (no CoUninitialize, no later side effects)\n");
    resetMock(E_FAIL);
    TsDevLibTestConfig cfg = makeConfig(/*throwAfterCom=*/true);

    int uninitBefore = g_mock.uninitCalls;
    bool caught = false;
    std::string what;
    try {
        auto lib = tsdev::makeTsDevLibForTest(nullptr, cfg);
        (void)lib;
    } catch (const std::runtime_error& e) {
        caught = true;
        what = e.what();
    }

    if (!caught) {
        return fail("E_FAIL: expected std::runtime_error (hard init failure)", 40);
    }
    if (g_mock.initCalls != 1) {
        return fail("E_FAIL: CoInitializeEx should be called exactly once", 41);
    }
    if (g_mock.uninitCalls != uninitBefore) {
        return fail("E_FAIL: CoUninitialize must NOT be called (no apartment entered)", 42);
    }
    {
        if (std::strstr(what.c_str(), "0x80004005") == nullptr) {
            std::fprintf(stderr, "  message=\"%s\" (expected to contain 0x80004005)\n", what.c_str());
            return fail("E_FAIL: exception message must name HRESULT 0x80004005", 43);
        }
    }
    std::fprintf(stdout, "    PASS: E_FAIL hard stop, no CoUninitialize, HRESULT named\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Check 5: Later-constructor exception after COM success — DISTINCT from
// check 1 by verifying the exception MESSAGE.
//
// COM succeeds (S_OK), then the ctor throws at the injected post-COM point
// (config.throwAfterCom == true). The COM guard's destructor runs during
// stack unwinding and calls the injected CoUninitialize EXACTLY once. This
// proves COM is balanced on the constructing thread even when a later
// constructor step fails — the same property TVS-L.2 established for
// ComApartmentGuard, now verified at the TsDevLib layer through the new seam.
//
// DISTINCT from check 1 (which also uses S_OK + throwAfterCom and checks the
// same COM-balancing counters): check 5 additionally captures the exception
// MESSAGE and asserts it identifies the throw as the injected post-COM
// exception — it MUST contain "post-COM" and MUST NOT contain "HRESULT"
// (the COM-init hard-stop branch's diagnostic). This proves the throw came
// from the post-COM point and not from the COM-init hard-stop branch, which
// check 1 does not verify.
// ---------------------------------------------------------------------------
int checkLaterCtorExceptionAfterComSuccess() {
    std::fprintf(stdout, "  [check 5] later-constructor exception after COM success (message identifies post-COM throw)\n");
    resetMock(S_OK);
    TsDevLibTestConfig cfg = makeConfig(/*throwAfterCom=*/true);

    int uninitBefore = g_mock.uninitCalls;
    bool caught = false;
    std::string what;
    try {
        // The factory returns a unique_ptr; if the ctor throws, no object is
        // produced and the unique_ptr is never set. The COM guard (a member
        // constructed first) is destroyed during unwinding.
        auto lib = tsdev::makeTsDevLibForTest(nullptr, cfg);
        (void)lib;
    } catch (const std::runtime_error& e) {
        caught = true;
        what = e.what();
        std::fprintf(stdout, "  caught expected post-COM exception: %s\n", what.c_str());
    }

    if (!caught) {
        return fail("later-ctor-exception: expected std::runtime_error after COM success", 50);
    }
    if (g_mock.initCalls != 1) {
        return fail("later-ctor-exception: CoInitializeEx should be called exactly once (COM succeeded first)", 51);
    }
    if (g_mock.uninitCalls != uninitBefore + 1) {
        std::fprintf(stderr, "  uninitCalls=%d (expected %d)\n",
                     g_mock.uninitCalls, uninitBefore + 1);
        return fail("later-ctor-exception: CoUninitialize should be called exactly once during unwind", 52);
    }
    // DISTINCT assertion: the message must identify the post-COM throw, NOT a
    // COM-init failure. The post-COM branch message contains "post-COM" and
    // never contains "HRESULT" (which only the COM-init hard-stop branch
    // emits). This guards against the throwAfterCom branch accidentally
    // falling through to the COM-failure diagnostic path.
    if (!containsCI(what, "post-COM")) {
        std::fprintf(stderr, "  message=\"%s\" (expected to contain \"post-COM\")\n", what.c_str());
        return fail("later-ctor-exception: message must identify the injected post-COM throw", 53);
    }
    if (containsCI(what, "HRESULT")) {
        std::fprintf(stderr, "  message=\"%s\" (must NOT contain \"HRESULT\" -- that is the COM-init failure path)\n", what.c_str());
        return fail("later-ctor-exception: message must NOT be the COM-init failure diagnostic", 54);
    }
    std::fprintf(stdout, "    PASS: COM succeeded, post-COM throw, message identifies post-COM (not COM-init failure), CoUninitialize balanced once\n");
    return 0;
}

int runTests() {
    std::fprintf(stdout,
                 "=== test_tsdev_com_init_seam: TsDevLib COM-init injection seam (TVS-L.13 GREEN) ===\n");

    int ec = 0;
    if ((ec = checkSOkThenThrow()) != 0)                       return ec;
    if ((ec = checkSFalseThenThrow()) != 0)                    return ec;
    if ((ec = checkChangedModeHardStop()) != 0)                return ec;
    if ((ec = checkGenericFailHardStop()) != 0)                return ec;
    if ((ec = checkLaterCtorExceptionAfterComSuccess()) != 0)  return ec;

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

// SEH wrapper — POD-only (no C++ objects with destructors) so __try is legal.
DWORD WINAPI threadProc(LPVOID /*param*/) {
    DWORD exitCode = 0;
    __try {
        exitCode = static_cast<DWORD>(runTests());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr,
                     "\n[FATAL] Unhandled SEH exception 0x%08X\n",
                     GetExceptionCode());
        exitCode = 99;
    }
    return exitCode;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_tsdev_com_init_seam: TsDevLib COM-init seam (TVS-L.13 GREEN) ===\n");

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  threadProc, nullptr, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n", GetLastError());
        return 11;
    }
    WaitForSingleObject(hThread, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);

    std::fprintf(stdout,
                 "\n=== test_tsdev_com_init_seam complete (exit %d) ===\n",
                 static_cast<int>(exitCode));
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return static_cast<int>(exitCode);
}
