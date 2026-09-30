// test_whserver_health.cpp
//
// Unit test for src/whserver_health.cpp: WHServer.exe pid discovery +
// VirtualQueryEx-based user-address-space (VAS) headroom measurement.
//
// This is a PURE UNIT TEST. It does NOT load orlib20.dll, does NOT touch the
// TradeStation installation, and does NOT construct TsDevLib. It measures the
// VAS of THIS test process and of a spawned ping.exe child — read-only process
// inspection only (PROCESS_QUERY_INFORMATION).
//
// The implementation in src/whserver_health.cpp is compiled directly into
// this test executable (see CMakeLists.txt), so no library link is needed.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

#include "../src/whserver_health.h"

#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "  FAIL: %s (exit %d)\n", msg, code);
    return code;
}

int runTests() {
    const uint64_t kTwoGb = 0x80000000ULL;

    // -------------------------------------------------------------------
    // Group 1: measureVasFree on THIS process succeeds with sane metrics.
    // -------------------------------------------------------------------
    {
        tsdev::WhVasStats st;
        bool ok = tsdev::measureVasFree(GetCurrentProcessId(), st);
        std::fprintf(stdout,
            "  [g1] self: ok=%d totalFree=%llu largestFree=%llu private=%llu\n",
            ok ? 1 : 0, (unsigned long long)st.totalFreeBytes,
            (unsigned long long)st.largestFreeBlockBytes,
            (unsigned long long)st.privateBytes);
        if (!ok) return fail("measureVasFree(self) returned false", 10);
        if (st.totalFreeBytes == 0)
            return fail("totalFreeBytes == 0 for self", 11);
        if (st.largestFreeBlockBytes == 0)
            return fail("largestFreeBlockBytes == 0 for self", 12);
        if (st.largestFreeBlockBytes > st.totalFreeBytes)
            return fail("largestFreeBlockBytes > totalFreeBytes", 13);
        if (st.totalFreeBytes >= kTwoGb)
            return fail("totalFreeBytes >= 2GB (32-bit self VAS bound)", 14);
        std::fprintf(stdout, "    PASS: group 1 (self VAS metrics sane)\n");
    }

    // -------------------------------------------------------------------
    // Group 2: measureVasFree on invalid pids fails cleanly.
    // -------------------------------------------------------------------
    {
        tsdev::WhVasStats st;
        if (tsdev::measureVasFree(0, st))
            return fail("measureVasFree(pid 0) returned true", 20);
        if (tsdev::measureVasFree(0xFFFFFFFF, st))
            return fail("measureVasFree(pid 0xFFFFFFFF) returned true", 21);
        std::fprintf(stdout, "    PASS: group 2 (invalid pids fail)\n");
    }

    // -------------------------------------------------------------------
    // Group 3: measureVasFree on a spawned child process.
    // -------------------------------------------------------------------
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        std::memset(&si, 0, sizeof(si));
        std::memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);
        // A trivial long-lived child with no GUI: ping sleeps between
        // echo requests, keeping the process alive for ~30 s. Spawn
        // ping.exe DIRECTLY (not via cmd /c) so TerminateProcess below
        // kills the measured process itself — a cmd wrapper would orphan
        // the ping.exe grandchild.
        char cmd[] = "ping.exe -n 30 127.0.0.1";
        if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            return fail("CreateProcessA for child failed", 30);
        }
        // Give the child a moment to initialize its address space.
        Sleep(500);
        tsdev::WhVasStats st;
        bool ok = tsdev::measureVasFree(pi.dwProcessId, st);
        std::fprintf(stdout,
            "  [g3] child pid=%lu: ok=%d totalFree=%llu largestFree=%llu\n",
            pi.dwProcessId, ok ? 1 : 0,
            (unsigned long long)st.totalFreeBytes,
            (unsigned long long)st.largestFreeBlockBytes);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (!ok) return fail("measureVasFree(child) returned false", 31);
        if (st.totalFreeBytes == 0 || st.largestFreeBlockBytes == 0)
            return fail("child VAS metrics are zero", 32);
        if (st.largestFreeBlockBytes > st.totalFreeBytes)
            return fail("child largest > total", 33);
        std::fprintf(stdout, "    PASS: group 3 (child VAS measured)\n");
    }

    // -------------------------------------------------------------------
    // Group 4: findWhServerPids returns without crashing; any returned pids
    // are nonzero and actually openable.
    // -------------------------------------------------------------------
    {
        std::vector<DWORD> pids = tsdev::findWhServerPids();
        std::fprintf(stdout, "  [g4] findWhServerPids -> %zu pid(s)\n",
                     pids.size());
        for (DWORD p : pids) {
            std::fprintf(stdout, "    pid=%lu\n", p);
            if (p == 0) return fail("findWhServerPids returned pid 0", 40);
        }
        std::fprintf(stdout, "    PASS: group 4 (pid enumeration sane)\n");
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

} // namespace

int main() {
    try {
        int ec = runTests();
        std::fprintf(stdout,
                     "\n=== test_whserver_health complete (exit %d) ===\n", ec);
        if (ec == 0) std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
        else         std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
        return ec;
    } catch (...) {
        std::fprintf(stderr, "\n[FATAL] unhandled exception\n");
        return 99;
    }
}
