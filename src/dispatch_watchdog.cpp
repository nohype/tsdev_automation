// dispatch_watchdog.cpp — see dispatch_watchdog.h for the contract.
//
// Production bounded-wait containment (TVS-3): in-flight dispatch marker +
// 1 s polling watchdog + TerminateProcess(15) fire path. Compiled in BOTH
// the production tsdev_lib and the tsdev_lib_diag build (the TSDEV_DIAG_LOG
// records below compile out of production; everything else is identical).

#include "dispatch_watchdog.h"
#include "whserver_health.h"   // findWhServerPids()
#include "diag_log.h"          // TSDEV_DIAG_LOG (no-op without TSDEV_DIAG)

#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace tsdev {

namespace {

// --- In-flight marker state --------------------------------------------------
// Mutex-protected (any thread may enter/leave; the watchdog thread reads).
// Cost per dispatch: one mutex lock + a few stores — negligible against the
// multi-millisecond warehouse calls it brackets.
struct MarkerState {
    char     site[64];
    uint32_t aux;
    uint64_t enterQpc;
    unsigned depth;   // nested enters; >0 == in-flight
};

std::mutex  g_markerMutex;
MarkerState g_marker = { {}, 0u, 0ull, 0u };

// --- Watchdog state ----------------------------------------------------------
std::atomic<bool>     g_started{false};
std::atomic<unsigned> g_thresholdSecs{0};

// Exit code 15: next free code (0-11 CLI, 12 client markers, 13 diag
// watchdog, 14 whserver_vas_abort). See tsdev_cli.cpp's exit-code comment.
constexpr unsigned kDispatchWatchdogExitCode = 15;
constexpr DWORD    kPollMs = 1000;

// --- Fire path ----------------------------------------------------------------
// Truthful abort: say exactly which dispatch is wedged, how long it has been
// in-flight, the configured bound, and the detection reason — then die.
[[noreturn]] void fire(DispatchFireReason reason,
                       const DispatchMarkerSnapshot& marker,
                       double ageSecs, double thresholdSecs) {
    const char* reasonStr = dispatchFireReasonString(reason);
    if (reason == DispatchFireReason::DispatchTimeout) {
        TSDEV_DIAG_LOG("dispatch.watchdog.fired",
                       "site=%s aux=%u ageSecs=%.1f thresholdSecs=%.0f "
                       "reason=%s",
                       marker.site, marker.aux, ageSecs, thresholdSecs,
                       reasonStr);
    } else {
        TSDEV_DIAG_LOG("dispatch.watchdog.fastpath",
                       "site=%s aux=%u ageSecs=%.1f thresholdSecs=%.0f "
                       "reason=%s",
                       marker.site, marker.aux, ageSecs, thresholdSecs,
                       reasonStr);
    }
    TSDEV_DIAG_LOG("dispatch.watchdog.abort",
                   "exit=%u", kDispatchWatchdogExitCode);

    // stderr only: the emitLine channel is a CLI-main-thread lambda over
    // --pbar state and is NOT safe to invoke from the watchdog thread.
    std::fprintf(stderr,
                 "tsdev_cli: WATCHDOG ABORT: warehouse dispatch '%s' "
                 "aux=%u in-flight %.1fs (bound %.0fs) "
                 "[reason: %s]; exiting with code %u\n",
                 marker.site, static_cast<unsigned>(marker.aux), ageSecs,
                 thresholdSecs, reasonStr, kDispatchWatchdogExitCode);
    std::fflush(stderr);

    // TerminateProcess, NOT ExitProcess (raw form with explanatory comment,
    // AGENTS.md): ExitProcess runs DLL_PROCESS_DETACH for every loaded DLL,
    // and orlib20's detach path on a process holding an open warehouse
    // raises the BLOCKING dialog "TradeStation internal error, WHouse is
    // going to stay in memory (1)" (proven field incident — identical
    // reasoning to the diag watchdog fire() in diag_watchdog.cpp). stderr
    // is flushed above, so the process must die immediately WITHOUT running
    // any DLL detach, worker join, or closeWarehouse (which would itself
    // dispatch into the same wedged server).
    TerminateProcess(GetCurrentProcess(), kDispatchWatchdogExitCode);
    // TerminateProcess does not return; unreachable.
    std::abort();
}

// --- Watchdog thread ---------------------------------------------------------
DWORD WINAPI watchdogProc(LPVOID) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double thresholdSecs =
        static_cast<double>(g_thresholdSecs.load(std::memory_order_acquire));

    // Per-flight memory: WHServer must have been OBSERVED at least once
    // during the current flight before its disappearance can fire
    // whserver_gone (WHServer is a lazy singleton — absent while idle and
    // for a short window at flight start while the dispatch itself spawns
    // it), and the absence must persist across
    // kWhServerGoneConfirmSamples consecutive CONFIRMED-empty polls — a
    // failed Toolhelp32 snapshot is inconclusive (NEVER an empty sample)
    // and resets the streak via accountWhServerSample, so consecutive
    // snapshot failures can never kill a healthy in-flight dispatch.
    // Re-armed whenever a new flight's enterQpc is observed.
    uint64_t lastEnterQpc  = 0;
    bool     whSeen        = false;
    unsigned emptyWhSamples = 0;

    for (;;) {
        Sleep(kPollMs);

        DispatchMarkerSnapshot marker;
        dispatchSnapshot(marker);
        if (!marker.inFlight) {
            // Between dispatches: reset per-flight memory. Idle waits (e.g.
            // the serve broker blocked in ConnectNamedPipe) carry no
            // in-flight marker and are exempt by construction.
            lastEnterQpc   = 0;
            whSeen         = false;
            emptyWhSamples = 0;
            continue;
        }
        if (marker.enterQpc != lastEnterQpc) {
            lastEnterQpc   = marker.enterQpc;
            whSeen         = false;
            emptyWhSamples = 0;
        }

        // snapshotOk overload (NOT the collapsing one): a failed
        // Toolhelp32 snapshot is INCONCLUSIVE — it must never count as an
        // empty sample toward whserver_gone (consecutive failures
        // mid-flight would otherwise kill a HEALTHY in-flight dispatch).
        // Same fail-closed convention as the census/deathPoll
        // re-enumerations and the tscrash-enum below.
        std::vector<DWORD> whPids;
        const bool whSnapOk = findWhServerPids(whPids);
        if (!whSnapOk) {
            // Logged so the diag trail shows the whserver_gone fast path
            // was blind this poll rather than confirming absence.
            TSDEV_DIAG_LOG("dispatch.watchdog.wh-enum",
                           "snapshot inconclusive; whserver_gone blind");
        }
        accountWhServerSample(whSnapOk, whPids, whSeen, emptyWhSamples);
        std::vector<DWORD> crashParents;
        if (!findCrashReportParentPids(crashParents)) {
            // Inconclusive — NOT "no reporters": the whserver_crashed fast
            // path is blind this poll. Not-firing is the fail-safe
            // direction for a fire path (firing on an inconclusive set
            // could kill a healthy dispatch); the in-flight-age bound
            // remains the backstop for a wedged call. Logged so the diag
            // trail shows the fast path was blind rather than empty.
            TSDEV_DIAG_LOG("dispatch.watchdog.tscrash-enum",
                           "snapshot inconclusive; whserver_crashed blind");
        }

#ifdef TSDEV_DIAG
        // A TSCrashReport.exe whose parent is OUR pid is our own crash being
        // reported — never a fire condition; record it distinctly so the log
        // explains the nearby process death.
        for (DWORD p : crashParents) {
            if (p == GetCurrentProcessId()) {
                TSDEV_DIAG_LOG("dispatch.watchdog.tscrash-self",
                               "parent=own-pid; not a fire condition");
            }
        }
#endif

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const DispatchFireReason reason = decideDispatchFire(
            marker, static_cast<uint64_t>(now.QuadPart),
            static_cast<double>(freq.QuadPart), thresholdSecs,
            whPids, whSeen, emptyWhSamples, crashParents);
        if (reason != DispatchFireReason::None) {
            const double ageSecs =
                static_cast<double>(now.QuadPart -
                                    static_cast<LONGLONG>(marker.enterQpc)) /
                static_cast<double>(freq.QuadPart);
            fire(reason, marker, ageSecs, thresholdSecs);
            // Unreachable — fire() terminates the process.
        }
    }
}

} // namespace

// --- In-flight dispatch marker ------------------------------------------------

void dispatchEnter(const char* site, uint32_t aux) {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    std::lock_guard<std::mutex> lock(g_markerMutex);
    std::strncpy(g_marker.site, site ? site : "(unnamed)",
                 sizeof(g_marker.site) - 1);
    g_marker.site[sizeof(g_marker.site) - 1] = '\0';
    g_marker.aux      = aux;
    g_marker.enterQpc = static_cast<uint64_t>(qpc.QuadPart);
    g_marker.depth++;
}

void dispatchLeave() {
    std::lock_guard<std::mutex> lock(g_markerMutex);
    if (g_marker.depth > 0) g_marker.depth--;
}

void dispatchSnapshot(DispatchMarkerSnapshot& out) {
    std::lock_guard<std::mutex> lock(g_markerMutex);
    out.inFlight  = (g_marker.depth > 0);
    out.aux       = g_marker.aux;
    out.enterQpc  = g_marker.enterQpc;
    std::memcpy(out.site, g_marker.site, sizeof(out.site));
}

// --- Pure decision -------------------------------------------------------------

const char* dispatchFireReasonString(DispatchFireReason reason) {
    switch (reason) {
        case DispatchFireReason::DispatchTimeout: return "dispatch_timeout";
        case DispatchFireReason::WhServerCrashed: return "whserver_crashed";
        case DispatchFireReason::WhServerGone:    return "whserver_gone";
        default:                                  return "none";
    }
}

DispatchFireReason decideDispatchFire(
    const DispatchMarkerSnapshot& marker,
    uint64_t nowQpc, double qpcFreq, double thresholdSecs,
    const std::vector<DWORD>& whServerPids,
    bool whServerSeenDuringFlight,
    unsigned whServerEmptySamples,
    const std::vector<DWORD>& crashReportParentPids) {
    if (!marker.inFlight || qpcFreq <= 0.0) {
        return DispatchFireReason::None;
    }

    const double ageSecs =
        static_cast<double>(nowQpc - marker.enterQpc) / qpcFreq;

    // (1) Primary: in-flight dispatch older than the bound. Legit single
    // dispatches max out at ~3.4 s (140 healthy logs); the 300 s default is
    // a safe margin above legit and far below forever.
    if (thresholdSecs > 0.0 && ageSecs > thresholdSecs) {
        return DispatchFireReason::DispatchTimeout;
    }

    // (2) Fast path: TSCrashReport.exe running as a CHILD of a WHServer.exe
    // pid — TSCrash.dll launches the reporter from inside the crashing
    // process, so the wedged COM call will never return. Parents that are
    // not WHServer pids (another process's crash, including our own) do
    // NOT fire.
    for (DWORD parent : crashReportParentPids) {
        for (DWORD wh : whServerPids) {
            if (parent == wh) {
                return DispatchFireReason::WhServerCrashed;
            }
        }
    }

    // (3) Fast path: WHServer vanished mid-flight after having been seen —
    // dead is dead. WHServer never seen during this flight (lazy singleton
    // still spawning) is NOT a fire condition, and neither is a streak of
    // fewer than kWhServerGoneConfirmSamples empty samples: the caller
    // increments whServerEmptySamples ONLY on confirmed-empty polls (a
    // failed Toolhelp32 snapshot is inconclusive and resets the streak —
    // accountWhServerSample), so the absence must persist across
    // kWhServerGoneConfirmSamples consecutive confirmed-empty polls
    // before it counts.
    if (whServerSeenDuringFlight && whServerPids.empty() &&
        whServerEmptySamples >= kWhServerGoneConfirmSamples) {
        return DispatchFireReason::WhServerGone;
    }

    return DispatchFireReason::None;
}

void accountWhServerSample(bool snapshotOk,
                           const std::vector<DWORD>& whServerPids,
                           bool& whSeen, unsigned& emptyWhSamples) {
    if (!snapshotOk) {
        // Inconclusive poll — not an empty sample and not a sighting. The
        // confirmed-empty streak RESETS: whserver_gone requires
        // consecutive CONFIRMED-empty polls and a gap in confirmation
        // breaks consecutiveness (the census "inconclusive never counts"
        // discipline — whserver_census.cpp emptyStreak/absentStreak and
        // whserver_recycle.cpp deadStreak). whSeen keeps its per-flight
        // value: a failed snapshot is no evidence of presence either.
        emptyWhSamples = 0;
        return;
    }
    if (!whServerPids.empty()) {
        whSeen = true;
        emptyWhSamples = 0;
    } else {
        ++emptyWhSamples;
    }
}

bool findCrashReportParentPids(std::vector<DWORD>& parents) {
    parents.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"TSCrashReport.exe") == 0) {
                parents.push_back(pe.th32ParentProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    // Same ERROR_NO_MORE_FILES convention as findWhServerPids
    // (whserver_health.cpp): the clean end of both arms — a genuinely
    // empty snapshot on First, a completed walk on Next. Any other error
    // is a truncated enumeration — a partial set is never evidence of
    // "no crash reporters", so the result is inconclusive and carries no
    // set.
    const bool ok = (GetLastError() == ERROR_NO_MORE_FILES);
    CloseHandle(snap);
    if (!ok) parents.clear();
    return ok;
}

std::vector<DWORD> findCrashReportParentPids() {
    std::vector<DWORD> parents;
    findCrashReportParentPids(parents);  // enumeration failure collapses to
                                         // empty (legacy contract)
    return parents;
}

// --- Watchdog thread -----------------------------------------------------------

bool startDispatchWatchdog(unsigned thresholdSecs) {
    if (thresholdSecs == 0) return false;  // documented disable escape hatch
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) {
        return false;  // already started (spawn-once, stop-never)
    }
    g_thresholdSecs.store(thresholdSecs, std::memory_order_release);
    HANDLE h = CreateThread(nullptr, 0, watchdogProc, nullptr, 0, nullptr);
    if (!h) {
        g_started.store(false);
        return false;
    }
    CloseHandle(h);
    TSDEV_DIAG_LOG("dispatch.watchdog.started", "thresholdSecs=%u",
                   thresholdSecs);
    return true;
}

} // namespace tsdev
