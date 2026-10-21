// test_dispatch_watchdog.cpp
//
// Unit test for src/dispatch_watchdog.cpp (TVS-3 production bounded-wait
// containment): the in-flight dispatch marker (enter/leave/snapshot), the
// DispatchInFlightGuard RAII form (including exception-path cleanup),
// every branch of the pure fire decision decideDispatchFire(), and the
// pure per-poll WHServer-sample accounting accountWhServerSample()
// (an inconclusive Toolhelp32 snapshot never counts toward whserver_gone).
//
// The fire() side effect (stderr + TerminateProcess(15)) is NOT exercised —
// it cannot be unit-tested by design; the decision is pure and the fire
// path is covered by manual/E2E verification.
//
// This is a PURE UNIT TEST. It does NOT load orlib20.dll, does NOT touch
// the TradeStation installation, and does NOT construct TsDevLib.
// src/dispatch_watchdog.cpp + src/whserver_health.cpp are compiled directly
// into this test executable (see CMakeLists.txt).
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

#include "../src/dispatch_watchdog.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "  FAIL: %s (exit %d)\n", msg, code);
    return code;
}

// Build a marker snapshot for the pure-decision tests (bypasses the live
// marker: decision inputs are injected, matching the unit-test contract).
tsdev::DispatchMarkerSnapshot makeMarker(bool inFlight, const char* site,
                                         uint32_t aux, uint64_t enterQpc) {
    tsdev::DispatchMarkerSnapshot s;
    s.inFlight = inFlight;
    std::strncpy(s.site, site, sizeof(s.site) - 1);
    s.site[sizeof(s.site) - 1] = '\0';
    s.aux = aux;
    s.enterQpc = enterQpc;
    return s;
}

int runTests() {
    const double kFreq = 10000000.0;  // 10 MHz — canonical QPC frequency

    // -------------------------------------------------------------------
    // Group 1: marker enter/snapshot/leave semantics.
    // -------------------------------------------------------------------
    {
        tsdev::DispatchMarkerSnapshot s;
        tsdev::dispatchSnapshot(s);
        if (s.inFlight)
            return fail("marker in-flight before any enter", 10);

        tsdev::dispatchEnter("Linker", 0x1234);
        tsdev::dispatchSnapshot(s);
        if (!s.inFlight) return fail("marker not in-flight after enter", 11);
        if (std::strcmp(s.site, "Linker") != 0)
            return fail("snapshot site mismatch", 12);
        if (s.aux != 0x1234u) return fail("snapshot aux mismatch", 13);
        if (s.enterQpc == 0) return fail("snapshot enterQpc == 0", 14);

        // Nested enter: depth 2, innermost site wins.
        tsdev::dispatchEnter("Ord28", 7);
        tsdev::dispatchSnapshot(s);
        if (!s.inFlight) return fail("marker lost in-flight on nested enter", 15);
        if (std::strcmp(s.site, "Ord28") != 0)
            return fail("nested enter did not update site", 16);

        tsdev::dispatchLeave();
        tsdev::dispatchSnapshot(s);
        if (!s.inFlight)
            return fail("inner leave cleared in-flight (depth ignored)", 17);

        tsdev::dispatchLeave();
        tsdev::dispatchSnapshot(s);
        if (s.inFlight) return fail("marker still in-flight after leaves", 18);
        std::fprintf(stdout, "    PASS: group 1 (marker enter/snapshot/leave)\n");
    }

    // -------------------------------------------------------------------
    // Group 2: DispatchInFlightGuard RAII, including C++ exception paths.
    // -------------------------------------------------------------------
    {
        tsdev::DispatchMarkerSnapshot s;
        {
            tsdev::DispatchInFlightGuard g("SaveModule_New", 42);
            tsdev::dispatchSnapshot(s);
            if (!s.inFlight) return fail("guard did not enter marker", 20);
            if (std::strcmp(s.site, "SaveModule_New") != 0)
                return fail("guard site mismatch", 21);
            if (s.aux != 42u) return fail("guard aux mismatch", 22);
        }
        tsdev::dispatchSnapshot(s);
        if (s.inFlight)
            return fail("guard left marker in-flight at scope end", 23);

        bool caught = false;
        try {
            tsdev::DispatchInFlightGuard g("VerifyModule_New", 9);
            tsdev::dispatchSnapshot(s);
            if (!s.inFlight) return fail("guard2 did not enter marker", 24);
            throw 1;  // simulated exception inside the guarded dispatch
        } catch (...) {
            caught = true;
        }
        if (!caught) return fail("exception not thrown", 25);
        tsdev::dispatchSnapshot(s);
        if (s.inFlight)
            return fail("guard left marker in-flight on exception path", 26);
        std::fprintf(stdout, "    PASS: group 2 (RAII guard, exception safe)\n");
    }

    // -------------------------------------------------------------------
    // Group 3: decideDispatchFire — not-in-flight / within-bound => None.
    // -------------------------------------------------------------------
    {
        const std::vector<DWORD> noWh;
        const std::vector<DWORD> noCrash;
        const uint64_t t0 = 5ull * 10000000ull;  // arbitrary enter time

        // Not in-flight: nothing fires regardless of inputs.
        {
            tsdev::DispatchMarkerSnapshot idle =
                makeMarker(false, "Linker", 1, t0);
            tsdev::DispatchFireReason r = tsdev::decideDispatchFire(
                idle, t0 + 999999ull * 1000ull, kFreq, 300.0,
                std::vector<DWORD>{1111}, true, 0u, std::vector<DWORD>{1111});
            if (r != tsdev::DispatchFireReason::None)
                return fail("not-in-flight marker fired", 30);
        }

        // In-flight but young: no fire.
        {
            tsdev::DispatchMarkerSnapshot m =
                makeMarker(true, "Linker", 1, t0);
            tsdev::DispatchFireReason r = tsdev::decideDispatchFire(
                m, t0 + 2ull * 10000000ull /* +2s */, kFreq, 300.0,
                noWh, false, 0u, noCrash);
            if (r != tsdev::DispatchFireReason::None)
                return fail("young in-flight dispatch fired", 31);
        }

        // In-flight at exactly the bound: NOT fired (strict >).
        {
            tsdev::DispatchMarkerSnapshot m =
                makeMarker(true, "Linker", 1, t0);
            tsdev::DispatchFireReason r = tsdev::decideDispatchFire(
                m, t0 + 300ull * 10000000ull /* +300s */, kFreq, 300.0,
                noWh, false, 0u, noCrash);
            if (r != tsdev::DispatchFireReason::None)
                return fail("at-bound dispatch fired (must be strict >)", 32);
        }
        std::fprintf(stdout,
                     "    PASS: group 3 (idle/young/at-bound => no fire)\n");
    }

    // -------------------------------------------------------------------
    // Group 4: decideDispatchFire — timeout, disabled, fast paths.
    // -------------------------------------------------------------------
    {
        const uint64_t t0 = 5ull * 10000000ull;
        const uint64_t old = t0 + 301ull * 10000000ull;  // +301 s > 300 s

        // (a) Aged in-flight marker over the bound => dispatch_timeout.
        {
            tsdev::DispatchMarkerSnapshot m =
                makeMarker(true, "VerifyModule_New", 77, t0);
            tsdev::DispatchFireReason r = tsdev::decideDispatchFire(
                m, old, kFreq, 300.0, std::vector<DWORD>{}, false, 0u,
                std::vector<DWORD>{});
            if (r != tsdev::DispatchFireReason::DispatchTimeout)
                return fail("aged marker did not fire dispatch_timeout", 40);
        }

        // (b) Same aged marker but disabled bound (0) => no timeout fire.
        {
            tsdev::DispatchMarkerSnapshot m =
                makeMarker(true, "VerifyModule_New", 77, t0);
            tsdev::DispatchFireReason r = tsdev::decideDispatchFire(
                m, old, kFreq, 0.0, std::vector<DWORD>{}, false, 0u,
                std::vector<DWORD>{});
            if (r != tsdev::DispatchFireReason::None)
                return fail("disabled bound still fired", 41);
        }

        // (c) whserver_crashed fast path: a TSCrashReport parent pid that IS
        //     a WHServer pid fires immediately (age well under the bound).
        {
            tsdev::DispatchMarkerSnapshot m =
                makeMarker(true, "cmd0x0F", 5, t0);
            tsdev::DispatchFireReason r = tsdev::decideDispatchFire(
                m, t0 + 1ull * 10000000ull /* +1s */, kFreq, 300.0,
                std::vector<DWORD>{4321, 1234}, true, 0u,
                std::vector<DWORD>{9999, 1234});
            if (r != tsdev::DispatchFireReason::WhServerCrashed)
                return fail("WHServer-parented TSCrashReport did not fire", 42);
        }

        // (d) False-positive guard: TSCrashReport parented by a NON-WHServer
        //     pid (e.g. our own pid or an unrelated process) must NOT fire.
        {
            tsdev::DispatchMarkerSnapshot m =
                makeMarker(true, "cmd0x0F", 5, t0);
            tsdev::DispatchFireReason r = tsdev::decideDispatchFire(
                m, t0 + 1ull * 10000000ull, kFreq, 300.0,
                std::vector<DWORD>{1234}, true, 0u,
                std::vector<DWORD>{5555});
            if (r != tsdev::DispatchFireReason::None)
                return fail("non-WHServer-parented TSCrashReport fired", 43);
        }

        // (e) whserver_gone: WHServer was seen during this flight, and the
        //     pid set has been empty for kWhServerGoneConfirmSamples
        //     consecutive samples => fire.
        {
            tsdev::DispatchMarkerSnapshot m =
                makeMarker(true, "Ord28", 3, t0);
            tsdev::DispatchFireReason r = tsdev::decideDispatchFire(
                m, t0 + 1ull * 10000000ull, kFreq, 300.0,
                std::vector<DWORD>{}, true,
                tsdev::kWhServerGoneConfirmSamples, std::vector<DWORD>{});
            if (r != tsdev::DispatchFireReason::WhServerGone)
                return fail("vanished WHServer mid-flight did not fire", 44);
        }

        // (f) Lazy-singleton guard: WHServer absent but NEVER seen during
        //     this flight => no fire (server may still be spawning).
        {
            tsdev::DispatchMarkerSnapshot m =
                makeMarker(true, "Ord28", 3, t0);
            tsdev::DispatchFireReason r = tsdev::decideDispatchFire(
                m, t0 + 1ull * 10000000ull, kFreq, 300.0,
                std::vector<DWORD>{}, false,
                tsdev::kWhServerGoneConfirmSamples, std::vector<DWORD>{});
            if (r != tsdev::DispatchFireReason::None)
                return fail("never-seen WHServer fired whserver_gone", 45);
        }

        // (g) WHServer present + no crash reporter => healthy, no fire.
        {
            tsdev::DispatchMarkerSnapshot m =
                makeMarker(true, "Ord28", 3, t0);
            tsdev::DispatchFireReason r = tsdev::decideDispatchFire(
                m, t0 + 1ull * 10000000ull, kFreq, 300.0,
                std::vector<DWORD>{1234}, true, 0u, std::vector<DWORD>{});
            if (r != tsdev::DispatchFireReason::None)
                return fail("healthy in-flight dispatch fired", 46);
        }

        // (h) Transient-snapshot gate: WHServer seen during this flight,
        //     but the empty sample count is below kWhServerGoneConfirmSamples
        //     => no fire (a single failed/empty Toolhelp32 poll is
        //     inconclusive, NOT proof of a vanished server).
        for (unsigned n = 1; n < tsdev::kWhServerGoneConfirmSamples; ++n) {
            tsdev::DispatchMarkerSnapshot m =
                makeMarker(true, "Ord28", 3, t0);
            tsdev::DispatchFireReason r = tsdev::decideDispatchFire(
                m, t0 + 1ull * 10000000ull, kFreq, 300.0,
                std::vector<DWORD>{}, true, n, std::vector<DWORD>{});
            if (r != tsdev::DispatchFireReason::None)
                return fail("transient empty WHServer sample fired "
                            "whserver_gone", 47);
        }
        std::fprintf(stdout,
                     "    PASS: group 4 (timeout/disabled/crash/gone)\n");
    }

    // -------------------------------------------------------------------
    // Group 5: reason strings are the documented stderr tokens.
    // -------------------------------------------------------------------
    {
        if (std::strcmp(tsdev::dispatchFireReasonString(
                            tsdev::DispatchFireReason::DispatchTimeout),
                        "dispatch_timeout") != 0)
            return fail("DispatchTimeout string", 50);
        if (std::strcmp(tsdev::dispatchFireReasonString(
                            tsdev::DispatchFireReason::WhServerCrashed),
                        "whserver_crashed") != 0)
            return fail("WhServerCrashed string", 51);
        if (std::strcmp(tsdev::dispatchFireReasonString(
                            tsdev::DispatchFireReason::WhServerGone),
                        "whserver_gone") != 0)
            return fail("WhServerGone string", 52);
        std::fprintf(stdout, "    PASS: group 5 (reason strings)\n");
    }

    // -------------------------------------------------------------------
    // Group 6: startDispatchWatchdog(0) is the documented disable hatch.
    // -------------------------------------------------------------------
    {
        if (tsdev::startDispatchWatchdog(0))
            return fail("startDispatchWatchdog(0) returned true", 60);
        std::fprintf(stdout,
                     "    PASS: group 6 (threshold 0 disables watchdog)\n");
    }

    // -------------------------------------------------------------------
    // Group 7: marker snapshot cost sanity — thousands of enter/leave pairs
    // per second must be trivially cheap (production overhead bound).
    // -------------------------------------------------------------------
    {
        const DWORD start = GetTickCount();
        for (int i = 0; i < 100000; ++i) {
            tsdev::dispatchEnter("Ord28", static_cast<uint32_t>(i));
            tsdev::dispatchLeave();
        }
        const DWORD ms = GetTickCount() - start;
        std::fprintf(stdout,
                     "    [g7] 100000 enter/leave pairs in %lu ms\n",
                     static_cast<unsigned long>(ms));
        if (ms > 5000)
            return fail("marker overhead exceeds sanity bound", 70);
        tsdev::DispatchMarkerSnapshot s;
        tsdev::dispatchSnapshot(s);
        if (s.inFlight)
            return fail("marker left in-flight after churn", 71);
        std::fprintf(stdout, "    PASS: group 7 (marker overhead)\n");
    }

    // -------------------------------------------------------------------
    // Group 8: findCrashReportParentPids snapshotOk overload — a live
    // Toolhelp32 snapshot must report ok=true. The fail-closed direction
    // (a failed walk returning not-ok) needs a Toolhelp32 failure
    // injection seam that does not exist; the inconclusive-poll contract
    // is covered through the recycle deathPoll seam in
    // test_whserver_recycle.
    // -------------------------------------------------------------------
    {
        std::vector<DWORD> parents;
        if (!tsdev::findCrashReportParentPids(parents))
            return fail("live crash-report snapshot must be ok", 80);
        std::fprintf(stdout,
                     "    PASS: group 8 (crash enum snapshotOk contract)\n");
    }

    // -------------------------------------------------------------------
    // Group 9: accountWhServerSample — an inconclusive WHServer snapshot
    // never advances emptyWhSamples and never fires whserver_gone (the
    // P4 gap-(f)(iii) fix). The Toolhelp32 call itself is NOT injectable
    // (watchdogProc is anonymous-namespace and calls the snapshot
    // directly), so the per-poll accounting is factored into the pure
    // accountWhServerSample seam — same style as decideDispatchFire —
    // and driven here with scripted poll outcomes.
    // -------------------------------------------------------------------
    {
        const uint64_t t0 = 5ull * 10000000ull;
        const std::vector<DWORD> one{1234};
        const std::vector<DWORD> none;
        bool whSeen;
        unsigned emptyWhSamples;

        // (a) Conclusive non-empty poll: observes WHServer, clears streak.
        whSeen = false; emptyWhSamples = 7;
        tsdev::accountWhServerSample(true, one, whSeen, emptyWhSamples);
        if (!whSeen || emptyWhSamples != 0)
            return fail("conclusive non-empty poll misaccounted", 90);

        // (b) Inconclusive poll: streak RESETS to 0, whSeen untouched —
        //     and the pid set is not consulted (a failed enumeration
        //     carries no set; a non-empty set proves it is ignored).
        whSeen = true; emptyWhSamples = 2;
        tsdev::accountWhServerSample(false, one, whSeen, emptyWhSamples);
        if (emptyWhSamples != 0 || !whSeen)
            return fail("inconclusive poll misaccounted (seen)", 91);
        whSeen = false; emptyWhSamples = 2;
        tsdev::accountWhServerSample(false, none, whSeen, emptyWhSamples);
        if (emptyWhSamples != 0 || whSeen)
            return fail("inconclusive poll misaccounted (unseen)", 92);

        // (c) Confirmed-empty polls accumulate the streak.
        whSeen = true; emptyWhSamples = 0;
        tsdev::accountWhServerSample(true, none, whSeen, emptyWhSamples);
        tsdev::accountWhServerSample(true, none, whSeen, emptyWhSamples);
        if (emptyWhSamples != 2 || !whSeen)
            return fail("confirmed-empty polls did not accumulate", 93);

        // (d) Scripted flight replay — the exact P4 (f)(iii) scenario:
        //     seen -> empty -> empty -> SNAPSHOT FAILURE -> empty ->
        //     empty. Under the old collapsing overload the failure
        //     counted as a third consecutive empty and fired
        //     whserver_gone on a HEALTHY dispatch. Now the inconclusive
        //     poll breaks consecutiveness: after six polls the streak is
        //     only 2 and decideDispatchFire must NOT fire.
        whSeen = false; emptyWhSamples = 0;
        tsdev::accountWhServerSample(true, one,  whSeen, emptyWhSamples);
        tsdev::accountWhServerSample(true, none, whSeen, emptyWhSamples);
        tsdev::accountWhServerSample(true, none, whSeen, emptyWhSamples);
        tsdev::accountWhServerSample(false, none, whSeen, emptyWhSamples);
        tsdev::accountWhServerSample(true, none, whSeen, emptyWhSamples);
        tsdev::accountWhServerSample(true, none, whSeen, emptyWhSamples);
        if (emptyWhSamples != 2)
            return fail("post-gap empty streak must restart, got != 2", 94);
        {
            tsdev::DispatchMarkerSnapshot m =
                makeMarker(true, "Ord28", 3, t0);
            tsdev::DispatchFireReason r = tsdev::decideDispatchFire(
                m, t0 + 6ull * 10000000ull /* +6s */, kFreq, 300.0,
                none, whSeen, emptyWhSamples, std::vector<DWORD>{});
            if (r == tsdev::DispatchFireReason::WhServerGone)
                return fail("inconclusive gap fired whserver_gone "
                            "(the P4 false-fire)", 95);
            if (r != tsdev::DispatchFireReason::None)
                return fail("unexpected fire reason in gap replay", 96);
        }

        // (e) The gate still works fail-safe: one more CONSECUTIVE
        //     confirmed-empty poll completes the streak and
        //     whserver_gone DOES fire — the fast path is preserved, not
        //     disabled.
        tsdev::accountWhServerSample(true, none, whSeen, emptyWhSamples);
        if (emptyWhSamples != tsdev::kWhServerGoneConfirmSamples)
            return fail("streak did not reach confirm count", 97);
        {
            tsdev::DispatchMarkerSnapshot m =
                makeMarker(true, "Ord28", 3, t0);
            tsdev::DispatchFireReason r = tsdev::decideDispatchFire(
                m, t0 + 7ull * 10000000ull /* +7s */, kFreq, 300.0,
                none, whSeen, emptyWhSamples, std::vector<DWORD>{});
            if (r != tsdev::DispatchFireReason::WhServerGone)
                return fail("3 consecutive confirmed empties did not "
                            "fire whserver_gone", 98);
        }
        std::fprintf(stdout,
                     "    PASS: group 9 (inconclusive wh-snapshot never "
                     "counts toward whserver_gone)\n");
    }

    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // Manual E2E mode (NOT run by the unit-test runners, mirroring the
    // test_crash_handler --crash convention): starts the REAL watchdog with
    // a 1 s bound and raises the REAL in-flight marker forever — the
    // watchdog's 1 s poll then walks marker -> decideDispatchFire -> fire()
    // -> TerminateProcess(15). The parent observes exit code 15 plus the
    // "WATCHDOG ABORT" stderr line. This exercises the production wiring
    // without needing a real WHServer wedge.
    if (argc > 1 && std::strcmp(argv[1], "--fire") == 0) {
        tsdev::startDispatchWatchdog(1);
        tsdev::dispatchEnter("E2EWedge", 0xE2E);
        for (;;) Sleep(60000);  // simulated wedged dispatch (never returns)
        return 0;             // unreachable
    }
    std::fprintf(stdout, "test_dispatch_watchdog\n");
    int rc = runTests();
    if (rc == 0) {
        std::fprintf(stdout, "ALL CHECKS PASSED\n");
    }
    return rc;
}
