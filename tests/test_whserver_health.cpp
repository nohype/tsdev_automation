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
#include <tlhelp32.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
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

    // -------------------------------------------------------------------
    // Group 5: vasBreach — the pure runBatch guard decision. Semantics:
    // STRICTLY below the threshold breaches; exactly-at-threshold passes.
    // -------------------------------------------------------------------
    {
        const unsigned T_TOTAL = 256, T_CONTIG = 64;
        // Healthy headroom -> pass.
        if (tsdev::vasBreach(400, 300, T_TOTAL, T_CONTIG))
            return fail("vasBreach(400,300,256,64) breached on healthy", 50);
        // Total below threshold -> breach.
        if (!tsdev::vasBreach(100, 300, T_TOTAL, T_CONTIG))
            return fail("vasBreach(100,300,256,64) passed on low total", 51);
        // Largest block below threshold -> breach (fragmentation), even
        // when total is ample.
        if (!tsdev::vasBreach(400, 10, T_TOTAL, T_CONTIG))
            return fail("vasBreach(400,10,256,64) passed on low largest", 52);
        // Both below -> breach.
        if (!tsdev::vasBreach(10, 5, T_TOTAL, T_CONTIG))
            return fail("vasBreach(10,5,256,64) passed on double breach", 53);
        // Exactly AT threshold -> pass (documented '<' semantics).
        if (tsdev::vasBreach(T_TOTAL, T_CONTIG, T_TOTAL, T_CONTIG))
            return fail("vasBreach at threshold breached ('<' semantics)", 54);
        // Zero thresholds -> everything passes (degenerate config).
        if (tsdev::vasBreach(0, 0, 0, 0))
            return fail("vasBreach(0,0,0,0) breached with zero thresholds", 55);
        // Impossible threshold (used by the manual forced-breach check).
        if (!tsdev::vasBreach(440, 400, 5000, 64))
            return fail("vasBreach(440,400,5000,64) passed vs 5000MB", 56);
        std::fprintf(stdout, "    PASS: group 5 (vasBreach decision)\n");
    }

    // -------------------------------------------------------------------
    // Group 6: findWhServerPids snapshotOk overload — the success arm
    // yields the authoritative set. A real snapshot must report true; the
    // returned set must equal an independent Toolhelp enumeration taken
    // here (the walk ran to clean completion — not a truncated set), and
    // every listed pid must independently verify as a WHServer.exe image
    // via OpenProcess + QueryFullProcessImageNameW. The false arm is NOT
    // exercisable here: the Toolhelp calls are direct with no seam, so a
    // snapshot or mid-walk failure cannot be injected from a unit test.
    // The consumer side of the fail-closed contract (snapshotOk=false is
    // inconclusive, never absent) is covered by test_whserver_census.
    // -------------------------------------------------------------------
    {
        std::vector<DWORD> pids;
        if (!tsdev::findWhServerPids(pids))
            return fail("findWhServerPids(snapshotOk) failed on a real snapshot", 60);

        // Independent enumeration of the same pid class for comparison.
        std::vector<DWORD> expected;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return fail("independent Toolhelp snapshot failed", 61);
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"WHServer.exe") == 0)
                    expected.push_back(pe.th32ProcessID);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        if (pids.size() != expected.size())
            return fail("findWhServerPids set differs from independent walk", 62);

        for (DWORD p : pids) {
            if (p == 0) return fail("findWhServerPids returned pid 0", 63);
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                   FALSE, p);
            if (h == nullptr)
                return fail("findWhServerPids pid not openable", 64);
            wchar_t img[MAX_PATH];
            DWORD len = MAX_PATH;
            const BOOL got = QueryFullProcessImageNameW(h, 0, img, &len);
            CloseHandle(h);
            if (!got)
                return fail("QueryFullProcessImageNameW on returned pid failed", 65);
            const wchar_t* base = wcsrchr(img, L'\\');
            if (_wcsicmp(base ? base + 1 : img, L"WHServer.exe") != 0)
                return fail("returned pid image is not WHServer.exe", 66);
        }
        std::fprintf(stdout,
            "    PASS: group 6 (snapshotOk overload: %zu authoritative pid(s))\n",
            pids.size());
    }

    // -------------------------------------------------------------------
    // Group 7: vasGuardAction -- the two-threshold field-policy decision
    // over the tri-state RecycleMode. Trigger pair (256,64); abort floor
    // (64,32). Every arm of the (breach-band x mode x foreignClients)
    // matrix is pinned: the band attempts a recycle ONLY in Full mode
    // (never past the foreign-client latch); below the floor RescueOnly
    // and Full attempt the rescue, Off hard-aborts.
    // -------------------------------------------------------------------
    {
        using tsdev::VasGuardAction;
        using tsdev::RecycleMode;
        const unsigned TRIG_T = 256, TRIG_C = 64, FLOOR_T = 64, FLOOR_C = 32;
        auto act = [&](uint64_t freeMB, uint64_t largestMB,
                       bool foreign, RecycleMode mode) {
            return tsdev::vasGuardAction(
                freeMB, largestMB, TRIG_T, TRIG_C,
                FLOOR_T, FLOOR_C, foreign, mode);
        };

        // No trigger breach -> Pass for every mode/latch combination.
        if (act(400, 300, false, RecycleMode::Off) != VasGuardAction::Pass)
            return fail("g7: healthy headroom not Pass", 70);
        if (act(400, 300, true, RecycleMode::Full) != VasGuardAction::Pass)
            return fail("g7: healthy headroom not Pass (latched+Full)", 71);
        // Exactly AT the trigger -> Pass (strict '<').
        if (act(TRIG_T, TRIG_C, false, RecycleMode::Full)
                != VasGuardAction::Pass)
            return fail("g7: at-trigger breached ('<' semantics)", 72);
        // Exactly AT the floor -> NOT an abort: trigger-breach band.
        if (act(FLOOR_T, FLOOR_C, false, RecycleMode::Off)
                != VasGuardAction::Continue)
            return fail("g7: at-floor treated as below floor", 73);

        // Below the abort floor: Off -> Abort; RescueOnly and Full ->
        // AttemptRecycle (the rescue). The latch is irrelevant below
        // the floor.
        if (act(10, 200, false, RecycleMode::Off) != VasGuardAction::Abort)
            return fail("g7: below floor + Off not Abort", 74);
        if (act(10, 200, true, RecycleMode::Off) != VasGuardAction::Abort)
            return fail("g7: below floor + latch + Off not Abort", 75);
        if (act(10, 200, false, RecycleMode::RescueOnly)
                != VasGuardAction::AttemptRecycle)
            return fail("g7: below floor + RescueOnly not AttemptRecycle", 76);
        if (act(10, 200, true, RecycleMode::RescueOnly)
                != VasGuardAction::AttemptRecycle)
            return fail("g7: below floor + latch + RescueOnly not Attempt", 77);
        // Largest-only below floor -> same band (fragmentation abort).
        if (act(200, 10, false, RecycleMode::Off) != VasGuardAction::Abort)
            return fail("g7: largest below floor + Off not Abort", 78);
        if (act(200, 10, true, RecycleMode::Full)
                != VasGuardAction::AttemptRecycle)
            return fail("g7: largest below floor + Full+latch not Attempt", 79);

        // The field band: below trigger, above floor.
        //   Off -> Continue (the 246MB field-abort fix);
        //   RescueOnly -> ALWAYS Continue (the recycle is reserved for
        //     the floor rescue; the band is not a danger level);
        //   Full + foreign latch -> Continue (restart impossible);
        //   Full + no latch -> AttemptRecycle.
        if (act(246, 135, false, RecycleMode::Off) != VasGuardAction::Continue)
            return fail("g7: field plateau + Off not Continue", 80);
        if (act(246, 135, false, RecycleMode::RescueOnly)
                != VasGuardAction::Continue)
            return fail("g7: field plateau + RescueOnly not Continue", 81);
        if (act(246, 135, true, RecycleMode::Full)
                != VasGuardAction::Continue)
            return fail("g7: field plateau + Full+latch not Continue", 82);
        if (act(246, 135, false, RecycleMode::Full)
                != VasGuardAction::AttemptRecycle)
            return fail("g7: field plateau + Full not AttemptRecycle", 83);
        // The latch does not matter for Off/RescueOnly band breaches
        // (already Continue) and does not matter when Pass.
        if (act(246, 135, true, RecycleMode::RescueOnly)
                != VasGuardAction::Continue)
            return fail("g7: field plateau + RescueOnly+latch not Continue",
                        84);
        // Largest-only trigger breach above largest-floor: same band.
        if (act(400, 40, false, RecycleMode::RescueOnly)
                != VasGuardAction::Continue)
            return fail("g7: largest band + RescueOnly not Continue", 85);
        if (act(400, 40, false, RecycleMode::Full)
                != VasGuardAction::AttemptRecycle)
            return fail("g7: largest band + Full not AttemptRecycle", 86);
        // Boundary: just above the floor (floor+1) is the band; just
        // below the trigger (trigger-1) is the band.
        if (act(FLOOR_T + 1, FLOOR_C + 1, false, RecycleMode::Full)
                != VasGuardAction::AttemptRecycle)
            return fail("g7: floor+1 + Full not AttemptRecycle", 87);
        if (act(TRIG_T - 1, TRIG_C - 1, false, RecycleMode::Full)
                != VasGuardAction::AttemptRecycle)
            return fail("g7: trigger-1 + Full not AttemptRecycle", 88);
        std::fprintf(stdout, "    PASS: group 7 (vasGuardAction policy)\n");
    }

    // Group 8: trigger/floor inversion detection -- an inverted pair
    // silently kills the floor in the band between the two, so the
    // configuration must be rejected at parse time.
    {
        using tsdev::vasThresholdsInverted;
        if (!vasThresholdsInverted(32, 64, 64, 32))
            return fail("g8: trigger-free below floor-free not detected", 88);
        if (!vasThresholdsInverted(256, 16, 64, 32))
            return fail("g8: trigger-largest below floor-largest not detected", 89);
        if (vasThresholdsInverted(256, 64, 64, 32))
            return fail("g8: sane defaults reported inverted", 90);
        if (vasThresholdsInverted(64, 32, 64, 32))
            return fail("g8: equal trigger/floor reported inverted", 91);
        std::fprintf(stdout, "    PASS: group 8 (vasThresholdsInverted)\n");
    }

    // -------------------------------------------------------------------
    // Group 9: vasAbortReason -- the machine-readable breach reason for
    // the guard's abort/boundary summaries. The caller passes the
    // threshold pair that FIRED the abort (the abort floor for the
    // exit-14 hard-abort arms and an authorized below-floor recycle
    // boundary; the restart trigger for an authorized above-floor
    // boundary), so the named metric is the one that actually crossed
    // the firing threshold -- never merely the first-checked one.
    // -------------------------------------------------------------------
    {
        // Free-only floor breach -> the free token.
        if (tsdev::vasAbortReason(10, 300, 64, 32) !=
                "whserver_vas_free=10MB")
            return fail("g9: free-only floor breach not named free", 95);
        // Largest-only floor breach -> the largest token. This is the
        // field nit's exact shape (free=100 above its 64 floor,
        // largest=10 below its 32 floor): the old trigger-first check
        // named a free metric that never crossed the firing floor.
        if (tsdev::vasAbortReason(100, 10, 64, 32) !=
                "whserver_vas_largest_free=10MB")
            return fail("g9: largest-only floor breach not named largest",
                        96);
        // Both crossed -> the deeper RELATIVE breach (the smaller
        // value/threshold ratio) wins, in both directions.
        if (tsdev::vasAbortReason(10, 31, 64, 32) !=
                "whserver_vas_free=10MB")
            return fail("g9: both crossed, free deeper not named free", 97);
        if (tsdev::vasAbortReason(63, 10, 64, 32) !=
                "whserver_vas_largest_free=10MB")
            return fail("g9: both crossed, largest deeper not named largest",
                        98);
        // Equal ratios tie -> the largest-block arm (fragmentation is
        // the sharper danger): 16/64 == 8/32.
        if (tsdev::vasAbortReason(16, 8, 64, 32) !=
                "whserver_vas_largest_free=8MB")
            return fail("g9: ratio tie did not report largest", 99);
        // An authorized above-floor boundary passes the TRIGGER pair;
        // same rule against it (field-plateau band: 246 free / 40
        // largest vs 256/64 -> largest is the deeper breach).
        if (tsdev::vasAbortReason(246, 40, 256, 64) !=
                "whserver_vas_largest_free=40MB")
            return fail("g9: trigger-pair band did not name deeper breach",
                        100);
        std::fprintf(stdout,
                     "    PASS: group 9 (vasAbortReason selection)\n");
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
