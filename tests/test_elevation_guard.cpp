// test_elevation_guard.cpp
//
// Unit test for TVS-K.1: elevation guard for all subcommands.
//
// This is a PURE UNIT TEST — it does NOT load orlib20.dll, does NOT touch the
// TradeStation installation, and does NOT need a 64 MB stack for orlib20. It
// exercises two file-local symbols in src/tsdev_cli.cpp:
//   - isElevated()           : the elevation-check helper.
//   - errorCodeString(11, ""): the "not_elevated" mapping for exit code 11.
//
// It runs on a worker thread with a 64 MB stack only to match the project's
// test harness convention (all tests run on a large-stack worker thread); the
// stack size is harmless for a unit test.
//
// Linkage mechanism:
//   isElevated() and errorCodeString() live inside an anonymous namespace in
//   src/tsdev_cli.cpp (internal linkage), so they are NOT accessible from a
//   separate translation unit. The only way to reach them is to #include the
//   .cpp directly so the test is part of the SAME translation unit. This pulls
//   in tsdev_cli.cpp's int main(); the test #defines TSDEV_CLI_TEST before the
//   include so main() is excluded (main() is wrapped in #ifndef TSDEV_CLI_TEST
//   in src/tsdev_cli.cpp).
//
// The historical TDD RED state is retained in Git history; this current test
// verifies the implemented helper and exit-code mapping.
//
// Checks:
//   1. isElevated() exists, returns a bool, and the value matches the test
//      runner's actual elevation state. Because isElevated() calls the real
//      Windows token APIs and is not mockable, the test independently queries
//      the process elevation token via the SAME Windows API sequence the
//      implementation will use (OpenProcessToken -> GetTokenInformation with
//      TokenElevation) and asserts isElevated() agrees with that ground truth.
//      When run elevated (the normal test-runner path) both return true; when
//      run non-elevated (CI) both return false.
//   2. errorCodeString(11, "") returns "not_elevated".
//   3. (TVS C) errorCodeString(12, <op>) returns "cancelled" — the
//      machine-readable code for the synthetic not-run markers appended
//      after a cooperative batch cancel (op-independent, like 10/11).
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

// Exclude tsdev_cli.cpp's main() so this test can define its own.
// (main() is wrapped in #ifndef TSDEV_CLI_TEST in src/tsdev_cli.cpp.)
#define TSDEV_CLI_TEST

// Include the CLI source directly so anonymous-namespace internals
// (isElevated, errorCodeString, CliResult, etc.) are visible.
#include "../src/tsdev_cli.cpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// Independently query the current process's elevation state using the SAME
// Windows token-API sequence the isElevated() implementation will use. This is
// the ground truth against which isElev() is compared. Returns true if the
// process token is elevated, false otherwise (including any API failure, which
// is treated as not-elevated to match the fail-safe fallback the plan
// specifies for isElevated()).
bool queryElevationGroundTruth() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return false;
    }
    TOKEN_ELEVATION elev = {};
    DWORD ret = 0;
    BOOL ok = GetTokenInformation(hToken, TokenElevation, &elev,
                                  sizeof(elev), &ret);
    CloseHandle(hToken);
    if (!ok || ret != sizeof(elev)) {
        return false;
    }
    return elev.TokenIsElevated != 0;
}

struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    std::fprintf(stdout,
                 "=== test_elevation_guard: isElevated() + errorCodeString(11) ===\n");

    // ------------------------------------------------------------------
    // Check 1: isElevated() exists, returns a bool, and matches the
    // runner's actual elevation state (ground truth via Windows token APIs).
    // ------------------------------------------------------------------
    {
        // Call the implemented isElevated() helper.
        bool elevated = isElevated();
        std::fprintf(stdout, "  isElevated() = %s\n",
                     elevated ? "true" : "false");

        // Ground truth from the same Windows token-API sequence.
        bool truth = queryElevationGroundTruth();
        std::fprintf(stdout, "  ground truth  = %s\n",
                     truth ? "true" : "false");

        if (elevated != truth) {
            ec = fail("isElevated() does not match ground-truth token query", 10);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: isElevated() matches runner elevation state\n");
    }

    // ------------------------------------------------------------------
    // Check 2: errorCodeString(11, "") returns "not_elevated".
    // ------------------------------------------------------------------
    {
        // Case 11 maps the non-elevated guard to "not_elevated".
        std::string s = errorCodeString(11, "");
        std::fprintf(stdout, "  errorCodeString(11, \"\") = \"%s\"\n",
                     s.c_str());
        if (s != "not_elevated") {
            ec = fail("errorCodeString(11, \"\") != \"not_elevated\"", 20);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: errorCodeString(11) == \"not_elevated\"\n");
    }

    // ------------------------------------------------------------------
    // Check 3 (TVS C): errorCodeString(12, <op>) returns "cancelled" for
    // every op — the not-run marker code is op-independent.
    // ------------------------------------------------------------------
    {
        std::string s1 = errorCodeString(12, "insert");
        std::string s2 = errorCodeString(12, "delete");
        std::fprintf(stdout,
                     "  errorCodeString(12, \"insert\") = \"%s\", "
                     "errorCodeString(12, \"delete\") = \"%s\"\n",
                     s1.c_str(), s2.c_str());
        if (s1 != "cancelled" || s2 != "cancelled") {
            ec = fail("errorCodeString(12, op) != \"cancelled\"", 30);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: errorCodeString(12) == \"cancelled\"\n");
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

// SEH wrapper — POD-only (no C++ objects with destructors) so __try is legal.
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
                 "=== test_elevation_guard: isElevated() + errorCodeString(11) (unit) ===\n");

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

    int exitCode = st.exitCode;
    std::fprintf(stdout,
                 "\n=== test_elevation_guard complete (exit %d) ===\n", exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
