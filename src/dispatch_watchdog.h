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
//        server has ever spawned. The watchdog uses the snapshotOk
//        findWhServerPids() overload: a failed Toolhelp32 snapshot is
//        INCONCLUSIVE — never evidence of absence — so whserver_gone only
//        fires after kWhServerGoneConfirmSamples CONSECUTIVE
//        CONFIRMED-empty samples. accountWhServerSample resets the streak
//        on an inconclusive poll (the "inconclusive never counts"
//        discipline of the census/deathPoll re-enumeration streaks), which
//        is also the "transient failure warns, doesn't abort" convention
//        of the VAS-headroom guard.
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

// Consecutive CONFIRMED-empty findWhServerPids() samples required before
// whserver_gone may fire. The watchdog feeds this counter through
// accountWhServerSample using the snapshotOk overload: a failed
// Toolhelp32 snapshot is inconclusive — it is NOT an empty sample and
// RESETS the streak — so the absence must be re-enumeration-confirmed on
// this many consecutive polls before `empty` reaches the decision as a
// fire condition.
constexpr unsigned kWhServerGoneConfirmSamples = 3;

// Pure decision: which (if any) containment condition holds right now.
// `marker` is a dispatchSnapshot() result; `nowQpc`/`qpcFreq` convert its
// enterQpc to an age; `whServerPids` is findWhServerPids();
// `whServerSeenDuringFlight` is the caller's memory of WHServer having been
// observed at least once since THIS flight began (prevents the lazy-
// singleton window — WHServer not yet spawned — from firing whserver_gone);
// `whServerEmptySamples` is the caller's count of CONSECUTIVE
// CONFIRMED-empty whServerPids samples during this flight
// (accountWhServerSample increments it only on snapshotOk-empty polls and
// resets it on inconclusive ones, so a transient snapshot failure cannot
// false-fire whserver_gone); `crashReportParentPids` is
// findCrashReportParentPids() (parent pid of each TSCrashReport.exe
// instance). Evaluation order: timeout (primary), then crashed, then gone.
DispatchFireReason decideDispatchFire(
    const DispatchMarkerSnapshot& marker,
    uint64_t nowQpc, double qpcFreq, double thresholdSecs,
    const std::vector<DWORD>& whServerPids,
    bool whServerSeenDuringFlight,
    unsigned whServerEmptySamples,
    const std::vector<DWORD>& crashReportParentPids);

// Pure per-poll WHServer-sample accounting for the watchdog loop, factored
// out so unit tests can pin the inconclusive-poll contract without
// injecting Toolhelp32 failures (same seam style as decideDispatchFire
// above and the census's decideCensus sample stream):
//   snapshotOk=false: the poll is INCONCLUSIVE — evidence in neither
//     direction. emptyWhSamples RESETS to 0 (whserver_gone requires
//     consecutive CONFIRMED-empty polls; a confirmation gap breaks the
//     streak) and whSeen is left unchanged.
//   snapshotOk=true, non-empty set: whSeen=true, emptyWhSamples=0.
//   snapshotOk=true, empty set: ++emptyWhSamples (a confirmed absence).
// The pid set is not consulted when snapshotOk is false (a failed
// enumeration carries no set).
void accountWhServerSample(bool snapshotOk,
                           const std::vector<DWORD>& whServerPids,
                           bool& whSeen, unsigned& emptyWhSamples);

// Parent pid of every running TSCrashReport.exe instance, via one Toolhelp32
// process snapshot (the same mechanism whserver_health.cpp uses; the
// PROCESSENTRY32 parent-pid field needs no extra APIs). Empty when none are
// running or the enumeration failed — callers that must distinguish "no
// reporters running" from "enumeration inconclusive" use the snapshotOk
// overload; a failed enumeration is NEVER evidence of no reporters.
std::vector<DWORD> findCrashReportParentPids();

// snapshotOk variant of the same enumeration: returns false ONLY when the
// enumeration is inconclusive — the Toolhelp32 snapshot itself failed, or
// the process walk did not run to clean completion (a Process32FirstW or
// Process32NextW failure whose GetLastError is not ERROR_NO_MORE_FILES; a
// truncated set is never evidence). On true, `parents` is the authoritative
// parent-pid set (possibly empty). On false, `parents` is cleared.
bool findCrashReportParentPids(std::vector<DWORD>& parents);

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
