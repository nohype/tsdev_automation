// test_cliresult_default.cpp
//
// RC-28 seam test: the CliResult default must never describe a lie. A
// worker that dies before filling its result (per-thread fatal
// containment re-raises; the UEF re-assert kills the process) leaves a
// default-constructed CliResult behind; emitting it must NOT produce a
// success=false + exit_code=0 frame (a failure frame with a success
// exit). The fail-safe default is exitCode=10 (thread_failure -- the same
// code the CreateThread-failure arms emit), so an unfilled result can
// never carry exit 0.
//
// Same-TU include (CliResult lives in src/tsdev_cli.cpp); TSDEV_CLI_TEST
// excludes its main(). PURE UNIT TEST: no orlib20, no TS install.

#define TSDEV_CLI_TEST
#include "../src/tsdev_cli.cpp"

#include <cstdio>
#include <string>
#include <windows.h>

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    // Check 1: the default is a failure frame, never exit 0. An
    // unfilled worker result (ListParams::result, ExistsParams::result,
    // and the same cmd*/run* pairs) is emitted verbatim; exit 0 on a
    // failed frame would be a lie.
    {
        const CliResult r;
        if (r.success) {
            ec = fail("check1: default CliResult must not be success",
                      10);
            return 0;
        }
        if (r.exitCode == 0) {
            ec = fail("check1: default CliResult exitCode must not be 0 "
                      "(an unfilled worker result would emit a failure "
                      "frame with a success exit)", 11);
            return 0;
        }
        if (r.exitCode != 10) {
            ec = fail("check1: default CliResult exitCode must be 10 "
                      "(thread_failure)", 12);
            return 0;
        }
        std::fprintf(stdout, "    default is failure exit 10 PASS\n");
    }

    // Check 2: the default exit maps to the thread_failure error code.
    {
        const CliResult r;
        if (errorCodeString(r.exitCode, "insert") != "thread_failure") {
            ec = fail("check2: default exit must map to thread_failure",
                      20);
            return 0;
        }
        if (errorCodeString(r.exitCode, "list") != "thread_failure") {
            ec = fail("check2: default exit must map to thread_failure "
                      "(list)", 21);
            return 0;
        }
        std::fprintf(stdout, "    default maps to thread_failure PASS\n");
    }

    // Check 3: the serialized default frame carries the failure exit
    // and an error object (success=false + exit_code=10, never
    // exit_code=0).
    {
        const CliResult r;
        const ordered_json j = cliResultToJson(r);
        if (!j.contains("success") ||
            j["success"].get<bool>() != false) {
            ec = fail("check3: default frame success != false", 30);
            return 0;
        }
        if (!j.contains("exit_code") ||
            j["exit_code"].get<int>() != 10) {
            ec = fail("check3: default frame exit_code != 10", 31);
            return 0;
        }
        if (!j.contains("error")) {
            ec = fail("check3: default frame missing error object", 32);
            return 0;
        }
        std::fprintf(stdout,
                      "    default frame is failure exit 10 PASS\n");
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

// SEH wrapper -- POD-only so __try is legal.
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
                 "=== test_cliresult_default: CliResult fail-safe default "
                 "===\n");

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
                 "\n=== test_cliresult_default complete (exit %d) ===\n",
                 exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
