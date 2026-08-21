// test_crash_handler.cpp
//
// Unit test + deliberate-crash test for the VEH-based crash handler
// (vehCrashHandler) added to src/tsdev_cli.cpp.
//
// This test has TWO modes:
//
//   1. Normal mode (no args): a PURE UNIT TEST. It verifies that the
//      vehCrashHandler symbol exists (by taking its address) and that
//      AddVectoredExceptionHandler is available (Windows API). It does NOT
//      trigger a crash. This mode is wired into run_unit_tests.ps1 as a
//      non-TS unit test (RequiresTs = $false).
//
//   2. Crash mode (--crash arg): a deliberate-crash test, NOT run as a unit
//      test. It is run by the standalone PowerShell wrapper
//      tests/test_crash_handler.ps1. In this mode the test installs the VEH,
//      suppresses the error mode, and deliberately dereferences a null
//      pointer to cause an EXCEPTION_ACCESS_VIOLATION (0xC0000005). If the
//      VEH works, the process exits with code 0xC0000005 (=-1073741819 as a
//      signed int32) and NO crash dialog (TSCrashReport.exe / WerFault.exe)
//      spawns. The PowerShell wrapper verifies the exit code AND that no
//      crash-reporter process is left running.
//
//      The crash mode does NOT use the 64 MB worker thread or the SEH
//      wrapper — we WANT the crash to happen and be caught by the VEH, not
//      swallowed by __try/__except.
//
// Linkage mechanism (normal mode only):
//   vehCrashHandler() lives inside an anonymous namespace in src/tsdev_cli.cpp
//   (internal linkage), so it is NOT accessible from a separate translation
//   unit. The only way to reach it is to #include the .cpp directly so the
//   test is part of the SAME translation unit. This pulls in tsdev_cli.cpp's
//   int main(); the test #defines TSDEV_CLI_TEST before the include so
//   main() is excluded (main() is wrapped in #ifndef TSDEV_CLI_TEST).
//
// Exit codes:
//   Normal mode: 0 only if ALL checks pass; distinct nonzero codes per failure.
//   Crash mode:  0xC0000005 (=-1073741819) if the VEH catches the access
//                violation and calls TerminateProcess. If the VEH fails,
//                the process may exit with a different code or spawn a crash
//                dialog (the PowerShell wrapper detects the latter).

// Exclude tsdev_cli.cpp's main() so this test can define its own.
// (main() is wrapped in #ifndef TSDEV_CLI_TEST at the end of tsdev_cli.cpp.)
#define TSDEV_CLI_TEST

// Include the CLI source directly so anonymous-namespace internals
// (vehCrashHandler, isElevated, errorCodeString, CliResult, etc.) are visible.
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

// --- Normal mode: pure unit test (no crash) ---------------------------------
int runNormalMode() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_crash_handler: vehCrashHandler symbol check (unit) ===\n");

    int ec = 0;

    // ------------------------------------------------------------------
    // Check 1: vehCrashHandler exists and is a callable function pointer.
    // Taking its address verifies the symbol is present in the translation
    // unit (it lives in the anonymous namespace of the included .cpp). We do
    // NOT call it with a real exception record — that would terminate the
    // process. We only verify the symbol resolves to a non-null function.
    // ------------------------------------------------------------------
    {
        auto handler = &vehCrashHandler;
        std::fprintf(stdout, "  &vehCrashHandler = %p\n",
                     reinterpret_cast<void*>(handler));

        // Verify it's a function pointer (not null). A function's address is
        // always non-null in C++; this guards against a missing definition.
        if (handler == nullptr) {
            ec = fail("vehCrashHandler is null", 10);
            return ec;
        }
        std::fprintf(stdout,
                     "    PASS: vehCrashHandler symbol exists and is non-null\n");
    }

    // ------------------------------------------------------------------
    // Check 2: AddVectoredExceptionHandler is available (Windows API).
    // It is declared in <windows.h> (errhandlingapi.h) and always present on
    // Windows. We verify the function pointer resolves so a missing SDK
    // header would be caught at test time rather than at first crash.
    // ------------------------------------------------------------------
    {
        auto api = &AddVectoredExceptionHandler;
        std::fprintf(stdout, "  &AddVectoredExceptionHandler = %p\n",
                     reinterpret_cast<void*>(api));

        if (api == nullptr) {
            ec = fail("AddVectoredExceptionHandler is null", 20);
            return ec;
        }
        std::fprintf(stdout,
                     "    PASS: AddVectoredExceptionHandler is available\n");
    }

    // ------------------------------------------------------------------
    // Check 3: Verify the VEH can be installed and removed without error.
    // This exercises the real AddVectoredExceptionHandler /
    // RemoveVectoredExceptionHandler path with vehCrashHandler as the
    // callback. No exception is raised — we only confirm the install/remove
    // round-trip succeeds (returns a non-null handle).
    // ------------------------------------------------------------------
    {
        PVOID veh = AddVectoredExceptionHandler(1, vehCrashHandler);
        std::fprintf(stdout, "  AddVectoredExceptionHandler -> %p\n", veh);

        if (veh == nullptr) {
            ec = fail("AddVectoredExceptionHandler returned NULL", 30);
            return ec;
        }
        std::fprintf(stdout,
                     "    PASS: VEH installed successfully\n");

        ULONG ok = RemoveVectoredExceptionHandler(veh);
        std::fprintf(stdout, "  RemoveVectoredExceptionHandler -> %lu\n", ok);
        if (ok == 0) {
            ec = fail("RemoveVectoredExceptionHandler failed", 40);
            return ec;
        }
        std::fprintf(stdout,
                     "    PASS: VEH removed successfully\n");
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

// --- Crash mode: deliberate access violation (NOT a unit test) --------------
//
// Installs the VEH, suppresses the error mode, and dereferences a null
// pointer. The VEH should catch the EXCEPTION_ACCESS_VIOLATION and call
// TerminateProcess(0xC0000005), so the process exits with code
// 0xC0000005 (=-1073741819) and NO crash dialog spawns.
//
// This function does NOT return under normal operation — the VEH terminates
// the process. If it DOES return (VEH failed to fire), we exit with a
// distinct code so the PowerShell wrapper can detect the failure.
int runCrashMode() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_crash_handler: deliberate access violation (--crash) ===\n");
    std::fprintf(stdout,
                 "  Installing VEH (vehCrashHandler)...\n");

    // Install the VEH with high priority (first=1) so it runs before any
    // other vectored handlers and before orlib20's unhandled exception filter.
    PVOID veh = AddVectoredExceptionHandler(1, vehCrashHandler);
    if (veh == nullptr) {
        std::fprintf(stderr,
                     "[FAIL] AddVectoredExceptionHandler returned NULL\n");
        return 50;
    }
    std::fprintf(stdout, "  VEH installed (%p)\n", veh);

    // Suppress the standard Windows crash dialog (WerFault.exe) as
    // belt-and-suspenders. This does NOT suppress TSCrashReport.exe — the
    // VEH is what prevents that by terminating before orlib20's filter runs.
    SetErrorMode(SetErrorMode(0) | SEM_NOGPFAULTERRORBOX);
    std::fprintf(stdout, "  Error mode set (SEM_NOGPFAULTERRORBOX)\n");

    std::fprintf(stdout,
                 "  Triggering access violation (null pointer dereference)...\n");
    std::fflush(stdout);

    // Deliberately cause an access violation by writing through a null
    // pointer. The volatile qualifier prevents the compiler from optimizing
    // the write away. The VEH should catch this and TerminateProcess with
    // EXCEPTION_ACCESS_VIOLATION (0xC0000005).
    volatile int* nullPtr = nullptr;
    *nullPtr = 42;  // ACCESS_VIOLATION — VEH should catch and terminate

    // If we reach here, the VEH did NOT fire — that is a failure.
    std::fprintf(stderr,
                 "[FAIL] VEH did not catch the access violation (reached line after crash)\n");
    return 60;
}

} // namespace

int main(int argc, char** argv) {
    // Crash mode: --crash arg triggers a deliberate access violation.
    if (argc > 1 && std::strcmp(argv[1], "--crash") == 0) {
        return runCrashMode();
    }

    // Normal mode: pure unit test (no crash).
    int exitCode = runNormalMode();
    std::fprintf(stdout,
                 "\n=== test_crash_handler complete (exit %d) ===\n", exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
