// test_crash_handler.cpp
//
// Unit test + deliberate-crash test for the own-process fatal-termination
// containment in src/tsdev_cli.cpp: the VEH-based crash handler
// (vehCrashHandler) plus the P4 gap-e guards (std::terminate, abort/
// SIGABRT, CRT invalid-parameter + purecall, and the process UEF).
//
// This test has TWO modes:
//
//   1. Normal mode (no args): a PURE UNIT TEST. It verifies that the
//      containment symbols exist (by taking their addresses), that
//      AddVectoredExceptionHandler is available, that the install helpers
//      run, AND — the self-crash suite — spawns CHILD copies of this same
//      executable with each deliberate-crash flag, asserting the child's
//      exit code and that no crash-reporter process (TSCrashReport.exe /
//      WerFault.exe) is left running parented by the child. The parent
//      never crashes itself. This mode is wired into run_unit_tests.ps1
//      as a non-TS unit test (RequiresTs = $false).
//
//   2. Crash modes (one arg): deliberate-crash children, each covering one
//      fatal channel:
//
//        --crash        Installs the VEH + error mode (the original
//                       minimal setup) and dereferences a null pointer ->
//                       VEH -> TerminateProcess(0xC0000005).
//        --abort        Installs the full production containment, then
//                       calls std::abort() -> SIGABRT handler ->
//                       TerminateProcess(21).
//        --terminate    Installs the full production containment, then
//                       calls std::terminate() directly (the same handler
//                       an escaping C++ exception reaches on frames that
//                       filter to terminate) -> TerminateProcess(20).
//        --throw        Installs the full production containment, then
//                       lets an ordinary C++ exception escape main
//                       unhandled. On this CRT the escape is delivered to
//                       the process UEF as SEH code 0xE06D7363 (MSVC C++
//                       exception) — NOT routed through std::terminate —
//                       so the expected death is the UEF's
//                       TerminateProcess(0xE06D7363). Proves the realistic
//                       "C++ exception escapes" channel dies silently.
//        --invalidparam Installs the full production containment, then
//                       trips a CRT invalid parameter (strcpy_s null
//                       source) -> TerminateProcess(22).
//        --purecall     Installs the full production containment, then
//                       performs a pure virtual call (virtual invoked
//                       during a base-class ctor) -> TerminateProcess(23).
//        --uef          Installs the full production containment, then
//                       installs a FOREIGN UEF over ours (simulating an
//                       orlib20/TSCrash.dll re-install), re-asserts ours
//                       via installProcessUef(), and raises a non-VEH SEH
//                       code -> our UEF -> TerminateProcess(<code>). If
//                       the foreign filter ever ran, the exit code would
//                       be its own marker instead.
//
//      Every mode installs the REAL production helpers
//      (installFatalContainment / installProcessUef) so the children
//      exercise the same code path main() installs. Expected exit codes
//      are the production constants (kExitFatal*) — the child never
//      returns normally; each mode's tail is unreachable. The crash modes
//      do NOT use the 64 MB worker thread or the SEH wrapper — we WANT the
//      crash to happen and reach the production guard under test.
//
//      The standalone wrapper tests/test_crash_handler.ps1 additionally
//      drives --crash end-to-end and sweeps the machine for stray
//      crash-reporter processes.
//
// Linkage mechanism (normal mode only):
//   The containment functions live inside an anonymous namespace in
//   src/tsdev_cli.cpp (internal linkage), so they are NOT accessible from a
//   separate translation unit. The only way to reach them is to #include
//   the .cpp directly so the test is part of the SAME translation unit.
//   This pulls in tsdev_cli.cpp's int main(); the test #defines
//   TSDEV_CLI_TEST before the include so main() is excluded (main() is
//   wrapped in #ifndef TSDEV_CLI_TEST).
//
// Exit codes:
//   Normal mode: 0 only if ALL checks pass; distinct nonzero codes per failure.
//   Crash modes: the channel's own production code (20/21/22/23) or the
//                SEH code (0xC0000005 / the raised code); any other code —
//                or a spawned crash reporter — is a failure detected by the
//                parent (this file) or the PowerShell wrapper.

// Exclude tsdev_cli.cpp's main() so this test can define its own.
// (main() is wrapped in #ifndef TSDEV_CLI_TEST at the end of tsdev_cli.cpp.)
#define TSDEV_CLI_TEST

// Include the CLI source directly so anonymous-namespace internals
// (vehCrashHandler, uefCrashHandler, installFatalContainment,
// installProcessUef, kExitFatal*, isElevated, errorCodeString, CliResult,
// etc.) are visible.
#include "../src/tsdev_cli.cpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <windows.h>
#include <tlhelp32.h>

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// ---------------------------------------------------------------------------
// Self-spawn machinery (normal mode): run this same executable with one
// crash-mode arg as a CHILD process, bounded-wait, return its exit code.
// The child deliberately dies; the parent asserts the production code and
// that no crash-reporter process was spawned for it — the same contract the
// standalone tests/test_crash_handler.ps1 wrapper asserts for --crash.
// ---------------------------------------------------------------------------

struct ChildResult {
    bool  spawned     = false;
    DWORD waitResult  = 0;
    DWORD exitCode    = 0;
    DWORD pid         = 0;
};

// Spawn our own exe with `arg`, wait up to timeoutMs, collect the exit code.
// On timeout the child is force-killed (a hung child means a dialog or a
// deadlock — either way the containment failed).
ChildResult runChildMode(const char* arg, DWORD timeoutMs) {
    ChildResult cr;
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) {
        return cr;
    }
    std::string cmd = "\"";
    cmd += exePath;
    cmd += "\" ";
    cmd += arg;

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi)) {
        return cr;  // spawned stays false
    }
    cr.spawned = true;
    cr.pid = pi.dwProcessId;
    cr.waitResult = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (cr.waitResult == WAIT_TIMEOUT) {
        // A surviving child means the guard did NOT fire (dialog/deadlock).
        // Kill it so the suite can report the failure instead of hanging.
        TerminateProcess(pi.hProcess, 97);
        WaitForSingleObject(pi.hProcess, 3000);
    }
    cr.exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &cr.exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return cr;
}

// Count crash-reporter processes (TSCrashReport.exe / WerFault.exe) whose
// PARENT pid is `parentPid` — the spawn signature proven in KB §14
// (TSCrash.dll launches the reporter from inside the crashing process;
// WerFault is likewise spawned by the faulting process on the WER path).
// Any found reporter is force-killed before counting continues so the
// machine is left clean. Returns -1 on snapshot failure (inconclusive —
// treated as a failure by the caller: never evidence of "no reporter").
int countCrashReportersForParent(DWORD parentPid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    int found = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ParentProcessID == parentPid &&
                (_wcsicmp(pe.szExeFile, L"TSCrashReport.exe") == 0 ||
                 _wcsicmp(pe.szExeFile, L"WerFault.exe") == 0)) {
                ++found;
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE,
                                       pe.th32ProcessID);
                if (h) {
                    TerminateProcess(h, 1);
                    CloseHandle(h);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Run one crash-mode child and assert the whole contract: spawned, exited
// (no timeout), expected exit code, and no crash-reporter process left
// behind. Returns 0 on PASS, or the failure's exit code via `codeOut`.
int expectChildDeath(const char* arg, DWORD expectedCode,
                     const char* label, int& codeOut) {
    codeOut = 0;
    std::fprintf(stdout, "  child %-14s expect exit %lu ...\n", arg,
                 static_cast<unsigned long>(expectedCode));

    ChildResult cr = runChildMode(arg, 15000);
    if (!cr.spawned) {
        std::fprintf(stderr, "[FAIL] %s: child did not spawn (%s, err=%lu)\n",
                     label, arg, GetLastError());
        codeOut = -1;
        return -1;
    }
    if (cr.waitResult != WAIT_OBJECT_0) {
        std::fprintf(stderr,
                     "[FAIL] %s: child did not exit within 15s (wait=%lu) -- "
                     "a dialog or deadlock escaped the containment\n",
                     label, static_cast<unsigned long>(cr.waitResult));
        codeOut = -2;
        return -2;
    }
    if (cr.exitCode != expectedCode) {
        std::fprintf(stderr,
                     "[FAIL] %s: child exit %lu != expected %lu\n",
                     label, static_cast<unsigned long>(cr.exitCode),
                     static_cast<unsigned long>(expectedCode));
        codeOut = -3;
        return -3;
    }

    // Give the OS a brief moment to spawn any crash reporter before
    // checking (same 500 ms convention as test_crash_handler.ps1).
    Sleep(500);
    const int reporters = countCrashReportersForParent(cr.pid);
    if (reporters != 0) {
        std::fprintf(stderr,
                     "[FAIL] %s: %d crash-reporter process(es) parented by "
                     "the child (snapshot %s) -- the death was not clean\n",
                     label, reporters,
                     reporters < 0 ? "failed (inconclusive)" : "succeeded");
        codeOut = -4;
        return -4;
    }

    std::fprintf(stdout,
                 "    PASS: %s child exited %lu, no crash reporters\n",
                 label, static_cast<unsigned long>(expectedCode));
    return 0;
}

// --- Normal mode: pure unit test (no crash) ---------------------------------
int runNormalMode() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_crash_handler: fatal-termination containment (unit) ===\n");

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

    // ------------------------------------------------------------------
    // Check 4 (P4 gap-e): every containment symbol exists. Same mechanism
    // as check 1 — address-of proves the definition is present; none of
    // these are safe to invoke with a live fatal condition (they terminate
    // the process), so presence is what is asserted here. The behavioral
    // proof is the child suite below.
    // ------------------------------------------------------------------
    {
        const void* symbols[] = {
            reinterpret_cast<const void*>(&uefCrashHandler),
            reinterpret_cast<const void*>(&onFatalTerminate),
            reinterpret_cast<const void*>(&onFatalSigabrt),
            reinterpret_cast<const void*>(&onFatalInvalidParameter),
            reinterpret_cast<const void*>(&onFatalPurecall),
            reinterpret_cast<const void*>(&installFatalContainment),
            reinterpret_cast<const void*>(&installThreadFatalContainment),
            reinterpret_cast<const void*>(&installProcessUef),
            reinterpret_cast<const void*>(&uefRearmSentryProc),
            reinterpret_cast<const void*>(&startUefRearmSentry),
        };
        const char* names[] = {
            "uefCrashHandler", "onFatalTerminate", "onFatalSigabrt",
            "onFatalInvalidParameter", "onFatalPurecall",
            "installFatalContainment", "installThreadFatalContainment",
            "installProcessUef", "uefRearmSentryProc", "startUefRearmSentry",
        };
        bool allPresent = true;
        for (size_t i = 0; i < sizeof(symbols) / sizeof(symbols[0]); ++i) {
            std::fprintf(stdout, "  &%-32s = %p\n", names[i], symbols[i]);
            if (symbols[i] == nullptr) {
                fail(names[i], 45);
                allPresent = false;
            }
        }
        if (!allPresent) {
            ec = fail("a containment symbol is missing", 45);
            return ec;
        }
        std::fprintf(stdout,
                     "    PASS: all containment symbols exist and are non-null\n");
    }

    // ------------------------------------------------------------------
    // Check 5 (P4 gap-e): the production install helpers run in THIS
    // process without error. installFatalContainment() installs the
    // per-process CRT guards (abort behavior, SIGABRT, terminate,
    // invalid-param, purecall); installProcessUef() installs our UEF
    // outermost. Both are idempotent and change only DEATH behavior —
    // safe to run in the test harness, and it means this test process is
    // itself guarded (a real bug here dies silently with a channel code).
    // ------------------------------------------------------------------
    {
        installFatalContainment();
        installProcessUef();
        std::fprintf(stdout,
                     "    PASS: installFatalContainment + installProcessUef "
                     "installed in the test process\n");
    }

    // ------------------------------------------------------------------
    // Check 6 (P4 gap-e): the self-crash child suite. Each child installs
    // the production guards and trips ONE fatal channel; the parent
    // asserts the production exit code, a bounded lifetime, and zero
    // crash-reporter processes parented by the child.
    // ------------------------------------------------------------------
    std::fprintf(stdout,
                 "\n  --- self-crash child suite (each child dies deliberately) ---\n");
    {
        struct {
            const char* arg;
            DWORD       code;
            const char* label;
        } cases[] = {
            // VEH path: null deref -> EXCEPTION_ACCESS_VIOLATION.
            { "--crash",        0xC0000005u,                 "VEH access violation" },
            // C++ terminate channel (direct std::terminate call).
            { "--terminate",    kExitFatalTerminate,         "std::terminate" },
            // Unhandled C++ exception escaping main — reaches the process
            // UEF as SEH 0xE06D7363 on this CRT (never std::terminate).
            { "--throw",        0xE06D7363u,                 "unhandled C++ escape" },
            // abort()/SIGABRT channel.
            { "--abort",        kExitFatalAbort,             "abort()/SIGABRT" },
            // CRT invalid-parameter channel.
            { "--invalidparam", kExitFatalInvalidParameter,  "CRT invalid parameter" },
            // Pure virtual call channel.
            { "--purecall",     kExitFatalPureCall,          "pure virtual call" },
            // UEF channel: a non-VEH code after a simulated orlib
            // re-install of a foreign filter, re-asserted by
            // installProcessUef.
            { "--uef",          0xE0000242u,                 "process UEF (non-VEH code)" },
        };
        int childFail = 0;
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            int codeOut = 0;
            if (expectChildDeath(cases[i].arg, cases[i].code,
                                 cases[i].label, codeOut) != 0) {
                // Map the child-suite failure onto a distinct test exit
                // code per case so the failing channel is identifiable.
                fail(cases[i].label, 50 + static_cast<int>(i));
                childFail = 50 + static_cast<int>(i);
                break;  // one failed channel is enough to reject the run
            }
        }
        if (childFail != 0) {
            ec = fail("self-crash child suite failed", childFail);
            return ec;
        }
        std::fprintf(stdout,
                     "    PASS: all child fatal channels died with their "
                     "production exit codes and no crash reporters\n");
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

// --- Shared install for the non-VEH crash modes ------------------------------
// Mirrors main(): the CRT guards plus our UEF outermost. The VEH is NOT
// installed in these modes — the channels under test bypass it by design,
// and the UEF is the production backstop for any SEH escape.
void installProductionContainment() {
    installFatalContainment();
    installProcessUef();
    std::fprintf(stdout, "  production containment installed "
                         "(abort behavior, SIGABRT, terminate, "
                         "invalid-param, purecall, UEF)\n");
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

// --- Crash mode: std::abort() -> SIGABRT handler -> exit 21 ------------------
// The production guards clear _WRITE_ABORT_MSG | _CALL_REPORTFAULT, so
// abort() emits no message and invokes no WER; raise(SIGABRT) then reaches
// onFatalSigabrt -> TerminateProcess(kExitFatalAbort), skipping the
// _exit(3) -> ExitProcess -> DLL-detach tail entirely.
int runAbortMode() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_crash_handler: deliberate abort (--abort) ===\n");
    installProductionContainment();
    std::fprintf(stdout, "  Calling std::abort() ...\n");
    std::fflush(stdout);
    std::abort();  // SIGABRT -> onFatalSigabrt -> TerminateProcess(21)

    // Unreachable — the SIGABRT handler terminates the process.
    std::fprintf(stderr, "[FAIL] std::abort() returned\n");
    return 61;
}

// --- Crash mode: std::terminate() -> terminate handler -> exit 20 ------------
// std::terminate invokes the CURRENT per-thread terminate handler — ours
// (onFatalTerminate, installed by installFatalContainment's
// installThreadFatalContainment call) -> TerminateProcess(kExitFatalTerminate).
// This is the direct exercise of the terminate channel: the same handler an
// escaping C++ exception reaches on frames that filter to terminate.
int runTerminateMode() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_crash_handler: deliberate terminate (--terminate) ===\n");
    installProductionContainment();
    std::fprintf(stdout,
                 "  Calling std::terminate() ...\n");
    std::fflush(stdout);
    std::terminate();  // -> onFatalTerminate -> TerminateProcess(20)

    // Unreachable — the terminate handler terminates the process.
    std::fprintf(stderr, "[FAIL] std::terminate() returned\n");
    return 62;
}

// --- Crash mode: unhandled C++ exception escape -> UEF -> exit 0xE06D7363 ----
// An ordinary throw escaping every frame is a regular UNHANDLED exception.
// Empirically on this CRT it is delivered to the process UEF as the MSVC
// C++ exception code 0xE06D7363 — it does NOT route through
// std::terminate — so the expected death is the production UEF's
// TerminateProcess(exception code): still silent, still no reporter, just
// a different (self-describing) exit code. No SEH dispatch reaches the VEH
// for this either (0xE06D7363 is not in the fatal list), which is exactly
// why the UEF exists.
[[noreturn]] void doEscapeThrow() {
    throw 42;
}
int runThrowMode() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_crash_handler: unhandled C++ escape (--throw) ===\n");
    installProductionContainment();
    std::fprintf(stdout,
                 "  Letting a C++ exception escape main unhandled -> "
                 "process UEF (0xE06D7363) ...\n");
    std::fflush(stdout);
    doEscapeThrow();  // unhandled -> uefCrashHandler -> exit 0xE06D7363

    // Unreachable — the UEF terminates the process.
    std::fprintf(stderr, "[FAIL] unhandled throw returned\n");
    return 66;
}

// --- Crash mode: CRT invalid parameter -> exit 22 ----------------------------
// strcpy_s with a nullptr source invokes the CRT invalid-parameter handler;
// the production handler (onFatalInvalidParameter) converts it into
// TerminateProcess(kExitFatalInvalidParameter) instead of the default
// _invoke_watson -> WER path.
int runInvalidParamMode() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_crash_handler: deliberate CRT invalid parameter (--invalidparam) ===\n");
    installProductionContainment();
    std::fprintf(stdout,
                 "  Calling strcpy_s with a nullptr source ...\n");
    std::fflush(stdout);
    char dst[8] = {0};
    const errno_t rc = strcpy_s(dst, sizeof(dst), nullptr);
    // Unreachable — the invalid-parameter handler terminates the process.
    std::fprintf(stderr,
                 "[FAIL] strcpy_s(nullptr) returned %d -- invalid-parameter "
                 "handler did not fire\n", static_cast<int>(rc));
    return 63;
}

// --- Crash mode: pure virtual call -> exit 23 --------------------------------
// Calling a virtual function during a base-class constructor dispatches
// through the BASE vtable, whose slot for the pure virtual holds _purecall;
// the production handler (onFatalPurecall) converts it into
// TerminateProcess(kExitFatalPureCall) instead of the default WER path.
struct PureCallBase {
    PureCallBase() { callF(); }
    void callF() { f(); }          // dispatches base vtable -> _purecall
    virtual void f() = 0;
};
struct PureCallDerived : PureCallBase {
    void f() override {}
};
int runPureCallMode() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_crash_handler: deliberate pure virtual call (--purecall) ===\n");
    installProductionContainment();
    std::fprintf(stdout,
                 "  Constructing an object whose base ctor calls a pure virtual ...\n");
    std::fflush(stdout);
    PureCallDerived d;  // base ctor -> f() -> _purecall -> TerminateProcess(23)
    (void)d;

    // Unreachable — the purecall handler terminates the process.
    std::fprintf(stderr, "[FAIL] pure virtual call returned\n");
    return 64;
}

// --- Crash mode: process UEF with a non-VEH code -> exit <raised code> -------
// A foreign UEF stands in for orlib20/TSCrash.dll RE-installing its filter
// over ours (the ordering hazard). installProcessUef() must re-assert ours
// outermost — the proof is the exit code: our filter dies with the raised
// SEH code; had the foreign filter run, the process would have exited 77.
LONG WINAPI foreignUef(PEXCEPTION_POINTERS) {
    ExitProcess(77);  // sentinel: proves THIS filter ran (must never happen)
}
int runUefMode() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_crash_handler: process UEF, non-VEH code (--uef) ===\n");
    installProductionContainment();

    // Simulate orlib20 installing its own UEF after ours (load-time or a
    // lazy re-install) — our filter is no longer outermost.
    SetUnhandledExceptionFilter(foreignUef);
    std::fprintf(stdout,
                 "  foreign UEF installed over ours (orlib re-install simulation)\n");

    // The production fix: re-assert ours outermost (same call every worker
    // makes after `new TsDevLib`, and the sentry makes once a second).
    installProcessUef();
    std::fprintf(stdout, "  installProcessUef() re-asserted ours\n");

    const DWORD code = 0xE0000242;  // non-VEH, custom, noncontinuable
    std::fprintf(stdout,
                 "  RaiseException(0x%08lX, NONCONTINUABLE) -> our UEF "
                 "should exit with this code\n",
                 static_cast<unsigned long>(code));
    std::fflush(stdout);
    RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr);

    // Unreachable — our UEF terminates the process with `code`.
    std::fprintf(stderr, "[FAIL] RaiseException returned\n");
    return 65;
}

} // namespace

int main(int argc, char** argv) {
    // Crash modes: one arg selects the deliberate-fatal channel.
    if (argc > 1) {
        if (std::strcmp(argv[1], "--crash") == 0)        return runCrashMode();
        if (std::strcmp(argv[1], "--abort") == 0)        return runAbortMode();
        if (std::strcmp(argv[1], "--terminate") == 0)    return runTerminateMode();
        if (std::strcmp(argv[1], "--throw") == 0)        return runThrowMode();
        if (std::strcmp(argv[1], "--invalidparam") == 0) return runInvalidParamMode();
        if (std::strcmp(argv[1], "--purecall") == 0)     return runPureCallMode();
        if (std::strcmp(argv[1], "--uef") == 0)          return runUefMode();
    }

    // Normal mode: pure unit test (no crash in THIS process — the
    // deliberate crashes all happen in spawned children).
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
