// dispatch_watchdog.h
//
// Production bounded-wait containment for orlib20 warehouse dispatches
// (TVS-3). Proven field failure: WHServer.exe wedged or crashed -> orlib20's
// synchronous COM call (WaitForSingleObject(INFINITE) inside its dispatcher)
// never returns -> the client hung forever -> the user killed processes ->
// warehouse cache corruption. No TS binary exposes a timeout/cancel API
// (zero RpcCancelThread / CoCancelCall imports), so the wedged call cannot
// be un-wedged. Containment = detect -> truthful abort -> TerminateProcess.
//
// Two pieces, BOTH in the production build (not TSDEV_DIAG-gated):
//
//   1. In-flight dispatch marker. Every orlib20 warehouse call helper in
//      tsdev_lib.cpp enters the marker (site string + uint32 aux + enter
//      QPC) and leaves on return — via __try/__finally wrappers so leave()
//      also runs on SEH/C++ unwinding (RAII objects are illegal inside the
//      __try helpers, MSVC C2712). DispatchInFlightGuard is the RAII form
//      for scopes that may NOT use __try. The marker is a mutex-protected
//      process-global singleton; cost is a mutex + two stores per dispatch.
//
//   2. Watchdog thread (startDispatchWatchdog). Polls once per second:
//      - marker in-flight AND age > threshold -> fire (dispatch_timeout)
//      - marker in-flight AND a TSCrashReport.exe process exists whose
//        PARENT pid is a WHServer.exe pid -> fire immediately
//        (whserver_crashed: TSCrash.dll launches the reporter as a child of
//        the crashing process; the COM call will never return). A
//        TSCrashReport.exe parented by any OTHER pid (including our own)
//        never fires this path.
//      - marker in-flight AND WHServer.exe was observed running during THIS
//        flight but has now vanished -> fire (whserver_gone). WHServer
//        absent between dispatches is normal (lazy singleton); the
//        whServerSeenDuringFlight gate prevents false fires before the
//        server has ever spawned. Additionally, findWhServerPids() cannot
//        distinguish a Toolhelp32 snapshot FAILURE from a genuine empty
//        result, so a single empty poll is inconclusive: whserver_gone only
//        fires after kWhServerGoneConfirmSamples CONSECUTIVE empty samples
//        (same "transient failure warns, doesn't abort" convention as the
//        VAS-headroom guard).
//      Idle waits (the serve broker blocked in ConnectNamedPipe) carry no
//      in-flight marker, so they are exempt automatically.
//
// Fire path: truthful abort on stderr, then TerminateProcess(15). NOT
// ExitProcess: DLL detach on a process holding an open warehouse raises the
// blocking "WHouse stays in memory" dialog (proven incident — see the
// identical comment at the diag watchdog's fire() in diag_watchdog.cpp).
// Exit code 15 is the next free code (0-11 CLI, 12 client markers, 13 diag
// watchdog, 14 whserver_vas_abort).
//
// The decision logic is factored into the pure function decideDispatchFire
// so unit tests can cover every branch without touching TerminateProcess;
// the fire() side effect itself is exercised only by manual/E2E paths.
//
// 32-bit Win32. kernel32 + toolhelp only — NO new .lib dependencies.

#ifndef TSDEV_DISPATCH_WATCHDOG_H
#define TSDEV_DISPATCH_WATCHDOG_H

#include <windows.h>

#include <cstdint>
#include <vector>

namespace tsdev {

// Immutable snapshot of the in-flight marker. `site` is a fixed-size copy so
// the snapshot stays printable after the mutex is released.
struct DispatchMarkerSnapshot {
    bool     inFlight;    // marker depth > 0
    char     site[64];    // dispatch site name (diag site naming, e.g. "Linker")
    uint32_t aux;         // site-specific aux value (category/progId/cmdId)
    uint64_t enterQpc;    // QueryPerformanceCounter at the most recent enter()
};

// --- In-flight dispatch marker (any thread) ---------------------------------
// enter/leave are nested-safe (depth counter): the site/aux/enterQpc of the
// most recent enter win, and the marker stays in-flight until the depth
// returns to zero. After a nested dispatch leaves, the still-in-flight outer
// dispatch's age is measured from the inner's enter QPC — its timeout is
// delayed by the time the outer ran before the inner entered (outer-enter to
// inner-enter gap), and an abort during the outer's remainder names the inner
// site.
void dispatchEnter(const char* site, uint32_t aux);
void dispatchLeave();
void dispatchSnapshot(DispatchMarkerSnapshot& out);

// RAII marker guard for scopes that may NOT use __try (MSVC C2712 forbids
// objects with destructors in functions containing __try — the orlib20 call
// helpers use __try/__finally + dispatchEnter/Leave instead).
class DispatchInFlightGuard {
public:
    DispatchInFlightGuard(const char* site, uint32_t aux) {
        dispatchEnter(site, aux);
    }
    ~DispatchInFlightGuard() { dispatchLeave(); }
    DispatchInFlightGuard(const DispatchInFlightGuard&) = delete;
    DispatchInFlightGuard& operator=(const DispatchInFlightGuard&) = delete;
};

// --- Fire decision (pure, unit-testable) ------------------------------------
enum class DispatchFireReason {
    None = 0,
    DispatchTimeout,    // "dispatch_timeout"  — in-flight age > bound
    WhServerCrashed,    // "whserver_crashed"  — TSCrashReport parent is a WHServer pid
    WhServerGone,       // "whserver_gone"     — WHServer pid vanished mid-flight
};

const char* dispatchFireReasonString(DispatchFireReason reason);

// Consecutive empty findWhServerPids() samples required before
// whserver_gone may fire. A single empty poll is inconclusive — a
// Toolhelp32 snapshot failure is indistinguishable from "WHServer absent"
// (findWhServerPids() returns empty in both cases), so the watchdog
// confirms the absence across this many consecutive polls before feeding
// `empty` to the decision as a fire condition.
constexpr unsigned kWhServerGoneConfirmSamples = 3;

// Pure decision: which (if any) containment condition holds right now.
// `marker` is a dispatchSnapshot() result; `nowQpc`/`qpcFreq` convert its
// enterQpc to an age; `whServerPids` is findWhServerPids();
// `whServerSeenDuringFlight` is the caller's memory of WHServer having been
// observed at least once since THIS flight began (prevents the lazy-
// singleton window — WHServer not yet spawned — from firing whserver_gone);
// `whServerEmptySamples` is the caller's count of CONSECUTIVE empty
// whServerPids samples during this flight (whserver_gone requires >=
// kWhServerGoneConfirmSamples so a transient snapshot failure cannot
// false-fire); `crashReportParentPids` is findCrashReportParentPids()
// (parent pid of each TSCrashReport.exe instance). Evaluation order:
// timeout (primary), then crashed, then gone.
DispatchFireReason decideDispatchFire(
    const DispatchMarkerSnapshot& marker,
    uint64_t nowQpc, double qpcFreq, double thresholdSecs,
    const std::vector<DWORD>& whServerPids,
    bool whServerSeenDuringFlight,
    unsigned whServerEmptySamples,
    const std::vector<DWORD>& crashReportParentPids);

// Parent pid of every running TSCrashReport.exe instance, via one Toolhelp32
// process snapshot (the same mechanism whserver_health.cpp uses; the
// PROCESSENTRY32 parent-pid field needs no extra APIs). Empty when none are
// running or the snapshot fails.
std::vector<DWORD> findCrashReportParentPids();

// --- Watchdog thread ---------------------------------------------------------
// Start the 1-second polling watchdog. `thresholdSecs` is the in-flight
// bound (production default 300 s — far above the ~3.4 s legit maximum
// single-dispatch duration, far below forever). 0 disables the watchdog
// entirely (documented escape hatch). Spawn-once: subsequent calls are
// no-ops returning false; the thread is never joined (process exit or the
// fire path kills it). Returns false when disabled/already started/on
// CreateThread failure.
bool startDispatchWatchdog(unsigned thresholdSecs);

} // namespace tsdev

#endif // TSDEV_DISPATCH_WATCHDOG_H
