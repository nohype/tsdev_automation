// test_com_apartment_guard.cpp
//
// Unit test (TDD) for TVS-L.2: the ComApartmentGuard RAII STA COM
// apartment guard declared in src/com_apartment_guard.h and implemented in
// src/com_apartment_guard.cpp.
//
// This is a PURE UNIT TEST. It does NOT load orlib20.dll, does NOT touch the
// TradeStation installation, and does NOT need a 64 MB stack. It exercises the
// guard exclusively through the injectable ComApi seam with MOCK function
// pointers, so the test process's real COM apartment state is NEVER touched
// (the real CoInitializeEx / CoUninitialize are never called by this test).
//
// It runs on a 64 MB-stack worker thread only to match the project's test
// harness convention (all tests run on a large-stack worker thread); the stack
// size is harmless for a unit test.
//
// TDD GREEN step:
//   src/com_apartment_guard.cpp (the implementation) exists and the guard's
//   out-of-line constructor, destructor, initialized(), and initResult() are
//   defined. This test links and passes (GREEN).
//
// Test cases (from docs/workstream_unknown_link_fix_plan.md TVS-L.2 acceptance
// criteria):
//   1. S_OK success        : initialized()==true, initResult()==S_OK,
//                            CoUninitialize called exactly once on destruction.
//   2. S_FALSE success     : initialized()==true, initResult()==S_FALSE,
//                            CoUninitialize called exactly once on destruction.
//   3. RPC_E_CHANGED_MODE  : initialized()==false, initResult()==RPC_E_CHANGED_MODE,
//                            CoUninitialize NOT called.
//   4. Generic failure     : initialized()==false, initResult()==E_FAIL,
//                            CoUninitialize NOT called.
//   5. Exactly-once (S_OK) : construct + destruct -> CoUninitialize exactly once.
//   6. Exactly-once (S_FALSE): construct + destruct -> CoUninitialize exactly once.
//   7. Constructor-unwind  : a wrapper struct whose first member is a
//                            ComApartmentGuard throws in its own constructor
//                            AFTER the guard is constructed; the guard's
//                            destructor runs during stack unwinding and
//                            CoUninitialize is called exactly once.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

#include "../src/com_apartment_guard.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
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
// test can reset the counters before exercising the guard. The mock
// CoInitializeEx returns whatever HRESULT the test installs in
// MockState::nextInitResult. The mock CoUninitialize increments the
// uninitialize call count. Neither mock touches the real COM apartment.
//
// The mock function pointers are stored in a ComApi and injected into the
// guard, so the guard never calls the real CoInitializeEx / CoUninitialize.
// ---------------------------------------------------------------------------

struct MockState {
    HRESULT nextInitResult = S_OK;   // what mockCoInitializeEx returns
    int     initCalls      = 0;      // count of mockCoInitializeEx calls
    int     uninitCalls    = 0;      // count of mockCoUninitialize calls
    LPVOID  lastReserved   = nullptr; // last pvReserved passed to init
    DWORD   lastCoInit     = 0;       // last dwCoInit passed to init
};

// Single shared mock state. Each test resets it before use. The guard is
// constructed and destroyed on the same (worker) thread, so no synchronization
// is needed.
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

// ---------------------------------------------------------------------------
// Check 1: S_OK success.
// ---------------------------------------------------------------------------
int checkSOkSuccess() {
    std::fprintf(stdout, "  [check 1] S_OK success\n");
    resetMock(S_OK);

    int uninitBefore = g_mock.uninitCalls;
    {
        ComApartmentGuard guard(mockApi());
        if (!guard.initialized()) {
            return fail("S_OK: initialized() should be true", 10);
        }
        if (guard.initResult() != S_OK) {
            std::fprintf(stderr, "  initResult=0x%08lX\n",
                         static_cast<unsigned long>(guard.initResult()));
            return fail("S_OK: initResult() should be S_OK", 11);
        }
        if (g_mock.initCalls != 1) {
            std::fprintf(stderr, "  initCalls=%d\n", g_mock.initCalls);
            return fail("S_OK: CoInitializeEx should be called once at construct", 12);
        }
        if (g_mock.lastReserved != nullptr) {
            return fail("S_OK: CoInitializeEx should be called with pvReserved=nullptr", 13);
        }
        if (g_mock.lastCoInit != COINIT_APARTMENTTHREADED) {
            std::fprintf(stderr, "  lastCoInit=0x%08lX\n",
                         static_cast<unsigned long>(g_mock.lastCoInit));
            return fail("S_OK: CoInitializeEx should be called with COINIT_APARTMENTTHREADED", 14);
        }
        if (g_mock.uninitCalls != uninitBefore) {
            return fail("S_OK: CoUninitialize should NOT be called before destruction", 15);
        }
    }
    if (g_mock.uninitCalls != uninitBefore + 1) {
        std::fprintf(stderr, "  uninitCalls=%d (expected %d)\n",
                     g_mock.uninitCalls, uninitBefore + 1);
        return fail("S_OK: CoUninitialize should be called exactly once on destruction", 16);
    }
    std::fprintf(stdout, "    PASS: S_OK success, CoUninitialize called exactly once\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Check 2: S_FALSE success.
// ---------------------------------------------------------------------------
int checkSFalseSuccess() {
    std::fprintf(stdout, "  [check 2] S_FALSE success\n");
    resetMock(S_FALSE);

    int uninitBefore = g_mock.uninitCalls;
    {
        ComApartmentGuard guard(mockApi());
        if (!guard.initialized()) {
            return fail("S_FALSE: initialized() should be true", 20);
        }
        if (guard.initResult() != S_FALSE) {
            std::fprintf(stderr, "  initResult=0x%08lX\n",
                         static_cast<unsigned long>(guard.initResult()));
            return fail("S_FALSE: initResult() should be S_FALSE", 21);
        }
        if (g_mock.initCalls != 1) {
            return fail("S_FALSE: CoInitializeEx should be called once at construct", 22);
        }
        if (g_mock.uninitCalls != uninitBefore) {
            return fail("S_FALSE: CoUninitialize should NOT be called before destruction", 23);
        }
    }
    if (g_mock.uninitCalls != uninitBefore + 1) {
        std::fprintf(stderr, "  uninitCalls=%d (expected %d)\n",
                     g_mock.uninitCalls, uninitBefore + 1);
        return fail("S_FALSE: CoUninitialize should be called exactly once on destruction (S_FALSE still balances)", 24);
    }
    std::fprintf(stdout, "    PASS: S_FALSE success, CoUninitialize called exactly once\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Check 3: RPC_E_CHANGED_MODE failure.
// ---------------------------------------------------------------------------
int checkChangedModeFailure() {
    std::fprintf(stdout, "  [check 3] RPC_E_CHANGED_MODE failure\n");
    resetMock(RPC_E_CHANGED_MODE);

    int uninitBefore = g_mock.uninitCalls;
    {
        ComApartmentGuard guard(mockApi());
        if (guard.initialized()) {
            return fail("RPC_E_CHANGED_MODE: initialized() should be false", 30);
        }
        if (guard.initResult() != RPC_E_CHANGED_MODE) {
            std::fprintf(stderr, "  initResult=0x%08lX\n",
                         static_cast<unsigned long>(guard.initResult()));
            return fail("RPC_E_CHANGED_MODE: initResult() should be RPC_E_CHANGED_MODE", 31);
        }
        if (g_mock.initCalls != 1) {
            return fail("RPC_E_CHANGED_MODE: CoInitializeEx should be called once at construct", 32);
        }
        if (g_mock.uninitCalls != uninitBefore) {
            return fail("RPC_E_CHANGED_MODE: CoUninitialize should NOT be called before destruction", 33);
        }
    }
    if (g_mock.uninitCalls != uninitBefore) {
        std::fprintf(stderr, "  uninitCalls=%d (expected %d)\n",
                     g_mock.uninitCalls, uninitBefore);
        return fail("RPC_E_CHANGED_MODE: CoUninitialize should NOT be called on destruction (no apartment entered)", 34);
    }
    std::fprintf(stdout, "    PASS: RPC_E_CHANGED_MODE failure, no CoUninitialize\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Check 4: Generic failure (E_FAIL).
// ---------------------------------------------------------------------------
int checkGenericFailure() {
    std::fprintf(stdout, "  [check 4] generic failure (E_FAIL)\n");
    resetMock(E_FAIL);

    int uninitBefore = g_mock.uninitCalls;
    {
        ComApartmentGuard guard(mockApi());
        if (guard.initialized()) {
            return fail("E_FAIL: initialized() should be false", 40);
        }
        if (guard.initResult() != E_FAIL) {
            std::fprintf(stderr, "  initResult=0x%08lX\n",
                         static_cast<unsigned long>(guard.initResult()));
            return fail("E_FAIL: initResult() should be E_FAIL", 41);
        }
        if (g_mock.initCalls != 1) {
            return fail("E_FAIL: CoInitializeEx should be called once at construct", 42);
        }
        if (g_mock.uninitCalls != uninitBefore) {
            return fail("E_FAIL: CoUninitialize should NOT be called before destruction", 43);
        }
    }
    if (g_mock.uninitCalls != uninitBefore) {
        std::fprintf(stderr, "  uninitCalls=%d (expected %d)\n",
                     g_mock.uninitCalls, uninitBefore);
        return fail("E_FAIL: CoUninitialize should NOT be called on destruction (no apartment entered)", 44);
    }
    std::fprintf(stdout, "    PASS: generic failure (E_FAIL), no CoUninitialize\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Check 5: Exactly-once balancing (S_OK).
// Constructs and fully destructs a guard, then asserts CoUninitialize was
// called exactly once (not zero, not twice).
// ---------------------------------------------------------------------------
int checkExactlyOnceSOk() {
    std::fprintf(stdout, "  [check 5] exactly-once balancing (S_OK)\n");
    resetMock(S_OK);

    // First construct+destruct to confirm exactly one uninit per cycle.
    {
        ComApartmentGuard guard(mockApi());
        // no-op; just construct and let scope end.
    }
    if (g_mock.uninitCalls != 1) {
        std::fprintf(stderr, "  first cycle uninitCalls=%d (expected 1)\n",
                     g_mock.uninitCalls);
        return fail("exactly-once S_OK: first cycle should call CoUninitialize exactly once", 50);
    }

    // Second construct+destruct in the same test process: counters are NOT
    // reset, so this verifies the second cycle adds exactly one more uninit
    // (total 2), proving each guard balances COM exactly once and there is no
    // double-call or missed-call drift across instances.
    {
        ComApartmentGuard guard(mockApi());
        // no-op
    }
    if (g_mock.uninitCalls != 2) {
        std::fprintf(stderr, "  second cycle uninitCalls=%d (expected 2)\n",
                     g_mock.uninitCalls);
        return fail("exactly-once S_OK: second cycle should call CoUninitialize exactly once (total 2)", 51);
    }
    std::fprintf(stdout, "    PASS: exactly-once balancing (S_OK), two cycles -> two uninits\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Check 6: Exactly-once balancing (S_FALSE).
// S_FALSE is a success that still requires exactly one CoUninitialize.
// ---------------------------------------------------------------------------
int checkExactlyOnceSFalse() {
    std::fprintf(stdout, "  [check 6] exactly-once balancing (S_FALSE)\n");
    resetMock(S_FALSE);

    {
        ComApartmentGuard guard(mockApi());
    }
    if (g_mock.uninitCalls != 1) {
        std::fprintf(stderr, "  first cycle uninitCalls=%d (expected 1)\n",
                     g_mock.uninitCalls);
        return fail("exactly-once S_FALSE: first cycle should call CoUninitialize exactly once", 60);
    }
    {
        ComApartmentGuard guard(mockApi());
    }
    if (g_mock.uninitCalls != 2) {
        std::fprintf(stderr, "  second cycle uninitCalls=%d (expected 2)\n",
                     g_mock.uninitCalls);
        return fail("exactly-once S_FALSE: second cycle should call CoUninitialize exactly once (total 2)", 61);
    }
    std::fprintf(stdout, "    PASS: exactly-once balancing (S_FALSE), two cycles -> two uninits\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Check 7: Constructor-unwind balancing.
//
// Simulates the TsDevLib constructor scenario: a wrapper struct holds a
// ComApartmentGuard as its FIRST member, and the wrapper's constructor throws
// AFTER the guard member is fully constructed. The guard's destructor must run
// during stack unwinding and call CoUninitialize exactly once.
//
// This mirrors the real risk: if any step after the guard's construction in
// TsDevLib's constructor throws (SetDllDirectoryW, LoadLibraryW, OrlibResolver,
// patch application, OrlibInit), the guard must still balance COM exactly once.
// ---------------------------------------------------------------------------
struct OwnerThatThrows {
    ComApartmentGuard guard;  // first member: constructed first
    explicit OwnerThatThrows(ComApi api, bool shouldThrow)
        : guard(api) {
        if (shouldThrow) {
            // Simulate a later constructor step (e.g. OrlibInit) failing.
            throw std::runtime_error("simulated later constructor failure");
        }
    }
};

int checkConstructorUnwind() {
    std::fprintf(stdout, "  [check 7] constructor-unwind balancing\n");
    resetMock(S_OK);

    int uninitBefore = g_mock.uninitCalls;
    bool caught = false;
    try {
        // The guard member is constructed (CoInitializeEx succeeds, S_OK),
        // then the OwnerThatThrows constructor body throws. Stack unwinding
        // must destroy the guard member and call CoUninitialize exactly once.
        OwnerThatThrows owner(mockApi(), /*shouldThrow=*/true);
        (void)owner;
    } catch (const std::runtime_error& e) {
        caught = true;
        std::fprintf(stdout, "  caught expected exception: %s\n", e.what());
    }
    if (!caught) {
        return fail("constructor-unwind: expected exception was not thrown", 70);
    }
    if (g_mock.uninitCalls != uninitBefore + 1) {
        std::fprintf(stderr, "  uninitCalls=%d (expected %d)\n",
                     g_mock.uninitCalls, uninitBefore + 1);
        return fail("constructor-unwind: CoUninitialize should be called exactly once during unwind", 71);
    }
    std::fprintf(stdout, "    PASS: constructor-unwind balancing, CoUninitialize called exactly once\n");

    // Also verify the non-throwing path balances exactly once, so the
    // presence of a throwing sibling constructor does not corrupt the guard.
    resetMock(S_OK);
    {
        OwnerThatThrows owner(mockApi(), /*shouldThrow=*/false);
        (void)owner;
    }
    if (g_mock.uninitCalls != 1) {
        std::fprintf(stderr, "  non-throwing path uninitCalls=%d (expected 1)\n",
                     g_mock.uninitCalls);
        return fail("constructor-unwind: non-throwing path should call CoUninitialize exactly once", 72);
    }
    std::fprintf(stdout, "    PASS: non-throwing sibling path also balances exactly once\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Check 8: Non-copyable / non-movable contract (compile-time).
//
// This is a static_assert-based check: if the guard were accidentally made
// copyable or movable, these asserts would fail to compile. They produce no
// runtime code, but they document and enforce the thread-affinity contract.
// ---------------------------------------------------------------------------
static_assert(!std::is_copy_constructible<ComApartmentGuard>::value,
              "ComApartmentGuard must be non-copyable (thread-affine COM apartment)");
static_assert(!std::is_copy_assignable<ComApartmentGuard>::value,
              "ComApartmentGuard must be non-copy-assignable (thread-affine COM apartment)");
static_assert(!std::is_move_constructible<ComApartmentGuard>::value,
              "ComApartmentGuard must be non-movable (thread-affine COM apartment)");
static_assert(!std::is_move_assignable<ComApartmentGuard>::value,
              "ComApartmentGuard must be non-move-assignable (thread-affine COM apartment)");

int runTests() {
    std::fprintf(stdout,
                 "=== test_com_apartment_guard: ComApartmentGuard (unit, injectable seam) ===\n");

    int ec = 0;

    if ((ec = checkSOkSuccess()) != 0)         return ec;
    if ((ec = checkSFalseSuccess()) != 0)      return ec;
    if ((ec = checkChangedModeFailure()) != 0) return ec;
    if ((ec = checkGenericFailure()) != 0)     return ec;
    if ((ec = checkExactlyOnceSOk()) != 0)     return ec;
    if ((ec = checkExactlyOnceSFalse()) != 0)  return ec;
    if ((ec = checkConstructorUnwind()) != 0)  return ec;

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
                 "=== test_com_apartment_guard: ComApartmentGuard (unit) ===\n");

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  threadProc, nullptr, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    WaitForSingleObject(hThread, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);

    std::fprintf(stdout,
                 "\n=== test_com_apartment_guard complete (exit %d) ===\n",
                 static_cast<int>(exitCode));
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return static_cast<int>(exitCode);
}
