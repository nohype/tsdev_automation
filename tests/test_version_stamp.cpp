// test_version_stamp.cpp
//
// Unit test for the version-stamp helpers in src/tsdev_cli.cpp (self-update
// design tmp/UPDATE_DESIGN.md section 6 + Appendix B):
//   - kTsdevCliVersion: the build-time stamp (TSDEV_VERSION_STRING with the
//     #ifndef "0.0.0-dev" fallback). This test compiles WITHOUT the define,
//     so the fallback path itself is what is exercised here.
//   - versionStampIsSemver(): strict X.Y.Z classification -- exactly two
//     dots separating three non-empty all-digit components.
//   - versionStampIsDev(): the inverse -- only a strict X.Y.Z stamp is a
//     release build; every other stamp reports dev_build:true.
//   - versionStampDisplay(): the v-prefix rule -- strict semver renders
//     "vX.Y.Z"; every other stamp renders verbatim.
//
// This is a PURE UNIT TEST -- it does NOT load orlib20.dll, does NOT touch
// the TradeStation installation, and does NOT need a 64 MB stack for
// orlib20. It runs on a worker thread with a 64 MB stack only to match the
// project's test harness convention (all tests run on a large-stack worker
// thread); the stack size is harmless for a unit test.
//
// Linkage mechanism:
//   The helpers live inside an anonymous namespace in src/tsdev_cli.cpp
//   (internal linkage), so they are NOT accessible from a separate
//   translation unit. The only way to reach them is to #include the .cpp
//   directly so the test is part of the SAME translation unit. This pulls
//   in tsdev_cli.cpp's int main(); the test #defines TSDEV_CLI_TEST before
//   the include so main() is excluded by the TSDEV_CLI_TEST guard in
//   src/tsdev_cli.cpp.
//
// Checks:
//   1. kTsdevCliVersion is non-empty (and, in this no-define build, is the
//      "0.0.0-dev" fallback).
//   2. "0.0.0-dev" classifies as a dev stamp (non-semver) and displays
//      verbatim.
//   3. Strict semver stamps ("0.1.6", "1.2.3", "0.0.0") classify as
//      non-dev and render with the "v" prefix.
//   4. Odd stamps ("abc", "1.2", "1.2.3.4", "v0.1.5", "", "1..2", "1.2.x")
//      classify as dev (non-semver) and render verbatim -- the
//      "everything unparseable is a development build" contract.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

// Exclude tsdev_cli.cpp's main() so this test can define its own.
// (main() is wrapped in the #ifndef TSDEV_CLI_TEST guard in tsdev_cli.cpp.)
#define TSDEV_CLI_TEST

// Include the CLI source directly so anonymous-namespace internals
// (kTsdevCliVersion, versionStampIsSemver, versionStampIsDev,
// versionStampDisplay) are visible.
#include "../src/tsdev_cli.cpp"

#include <cstdio>
#include <string>
#include <windows.h>

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// Assert one stamp's full classification + rendering in one place.
// Returns 0 on match, or prints a failure and returns `code`.
int checkStamp(const char* stamp, bool expectSemver, const char* expectDisplay,
               int code) {
    const bool semver = versionStampIsSemver(stamp);
    const bool dev = versionStampIsDev(stamp);
    const std::string display = versionStampDisplay(stamp);
    if (semver != expectSemver) {
        std::fprintf(stderr, "\n[FAIL] stamp \"%s\": semver=%d expected %d\n",
                     stamp, static_cast<int>(semver),
                     static_cast<int>(expectSemver));
        return code;
    }
    if (dev == expectSemver) {
        // dev must be the exact inverse of semver.
        std::fprintf(stderr, "\n[FAIL] stamp \"%s\": dev=%d is not the "
                     "inverse of semver=%d\n", stamp, static_cast<int>(dev),
                     static_cast<int>(semver));
        return code + 1;
    }
    if (display != expectDisplay) {
        std::fprintf(stderr, "\n[FAIL] stamp \"%s\": display \"%s\" "
                     "expected \"%s\"\n", stamp, display.c_str(),
                     expectDisplay);
        return code + 2;
    }
    std::fprintf(stdout, "    stamp %-10s semver=%d dev=%d display=\"%s\" "
                 "PASS\n", stamp, static_cast<int>(semver),
                 static_cast<int>(dev), display.c_str());
    return 0;
}

struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    // ------------------------------------------------------------------
    // Check 1: kTsdevCliVersion is non-empty; in this no-define build it
    // is the "0.0.0-dev" fallback stamp.
    // ------------------------------------------------------------------
    {
        std::fprintf(stdout, "  [1] kTsdevCliVersion = \"%s\"\n",
                     kTsdevCliVersion);
        if (kTsdevCliVersion[0] == '\0') {
            ec = fail("check1: kTsdevCliVersion is empty", 10);
            return 0;
        }
        if (std::string(kTsdevCliVersion) != "0.0.0-dev") {
            ec = fail("check1: no-define build did not fall back to "
                      "0.0.0-dev", 11);
            return 0;
        }
        std::fprintf(stdout, "    PASS: stamp non-empty (fallback "
                     "0.0.0-dev)\n");
    }

    // ------------------------------------------------------------------
    // Check 2: the "0.0.0-dev" fallback classifies dev (non-semver) and
    // renders verbatim.
    // ------------------------------------------------------------------
    if ((ec = checkStamp("0.0.0-dev", false, "0.0.0-dev", 20)) != 0) return 0;

    // ------------------------------------------------------------------
    // Check 3: strict semver stamps classify non-dev and render v-prefixed.
    // ------------------------------------------------------------------
    if ((ec = checkStamp("0.1.6",  true, "v0.1.6",  30)) != 0) return 0;
    if ((ec = checkStamp("1.2.3",  true, "v1.2.3",  33)) != 0) return 0;
    if ((ec = checkStamp("0.0.0",  true, "v0.0.0",  36)) != 0) return 0;
    if ((ec = checkStamp("10.20.30", true, "v10.20.30", 39)) != 0) return 0;

    // ------------------------------------------------------------------
    // Check 4: odd stamps classify dev (non-semver) and render verbatim --
    // only strict X.Y.Z earns the "v" prefix; everything else is a
    // development build.
    // ------------------------------------------------------------------
    if ((ec = checkStamp("abc",     false, "abc",     40)) != 0) return 0;
    if ((ec = checkStamp("1.2",     false, "1.2",     43)) != 0) return 0;
    if ((ec = checkStamp("1.2.3.4", false, "1.2.3.4", 46)) != 0) return 0;
    if ((ec = checkStamp("v0.1.5",  false, "v0.1.5",  49)) != 0) return 0;
    if ((ec = checkStamp("",        false, "",        52)) != 0) return 0;
    if ((ec = checkStamp("1..2",    false, "1..2",    55)) != 0) return 0;
    if ((ec = checkStamp("1.2.x",   false, "1.2.x",   58)) != 0) return 0;
    if ((ec = checkStamp(".1.2",    false, ".1.2",    61)) != 0) return 0;
    if ((ec = checkStamp("1.2.",    false, "1.2.",    64)) != 0) return 0;

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

// SEH wrapper -- POD-only (no C++ objects with destructors) so __try is legal.
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
                 "=== test_version_stamp: version-stamp helpers (unit) ===\n");

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
                 "\n=== test_version_stamp complete (exit %d) ===\n", exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
