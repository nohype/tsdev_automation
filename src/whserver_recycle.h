// whserver_recycle.h
//
// WHServer gated auto-recycle engine (tmp/RECYCLE_ENGINE_DESIGN.md §2 — R3
// "engine skeleton": steps 1-3 sequencing + ALL refuse exits, with the
// step-4 ForceShutDown attach behind an injectable IRecycleAttach seam and
// the step-3/step-5 process reads plus the step-2/step-4 zombie
// pre-checks behind an injectable IRecycleSampler seam; tests drive both
// with fakes under -NoTs).
//
// Trigger surface (§1): the engine fires ONLY at VAS-guard breach sites and
// only when --recycle-whserver authorized it; without the flag the breach
// path is byte-identical (exit 14). R1 site rule (PROVEN live finding):
// the engine may run only at NEVER-ATTACHED sites — an attached process
// contributes +7 to WHServer's m_nLock for the process's entire lifetime
// and closeWarehouse does NOT release it (only GRACEFUL process teardown
// does — R2, live-proven 2026-10-21: a TerminateProcess'd attached client
// leaves a corpse lock the resumed census refuses on), so an
// attached-site census can never pass. Sites: runBatch pre-flight (never
// attached — step 2 SKIPPED, recycle => continue in-process, the ordinary
// item-0 first-activation path per the F4 contract). At the runBatch
// between-items / runServe pre-mutation sites the caller checks
// recycleSiteDefers: with a live session (ATTACHED) it takes the same
// exit-18 boundary WITHOUT running the engine
// (reason=recycle_deferred_own_session) so the resumed process/broker
// runs the engine at its never-attached guard site; the never-attached
// edge of those sites (no op dispatched yet) still runs the engine
// in-process. R2: the attached-site defer now runs the §5 close stage
// itself (boundedCloseWarehouse below) before a GRACEFUL exit-18 — the
// only exit that releases the +7; a failed/bounded-out close refuses on
// the close-stage family (exit 19, terminateSelf).
//
// Fail-closed contract (§0, §5): the engine NEVER calls TerminateProcess on
// WHServer — the only recycle lever is the server's own
// IWHouse::ForceShutDown. Every stage failure returns a refusal; the
// caller's teardown contract is pinned in §5:
//   * close_failed (RefuseCloseFailed): the own session may still be OPEN —
//     emit + flush, then TerminateProcess-self(exit 19). NEVER `delete lib`
//     or ExitProcess into a possibly-open warehouse (the shipped watchdog
//     convention, dispatch_watchdog.cpp:79-88 — DLL detach raises the
//     blocking "WHouse stays in memory" dialog). The step-2 call itself is
//     bounded by a monitor thread (RecycleParams::closeBoundSecs): orlib20
//     binds CELWHouse per (thread, path) — the export is
//     _CloseWarehouseForCurrentThreadAndPath@4 — so the close can only run
//     on the engine's own thread, and a timed-out close leaves that thread
//     wedged inside a possibly-open-session call that can never return a
//     refuse. The monitor therefore applies THIS teardown itself (stderr +
//     TerminateProcess(19)); a non-fatal injected action (tests) instead
//     lets the engine refuse close_timeout on the same contract — the
//     late return is never trusted.
//   * ANY refuse taken while a session may still be open — i.e. a refuse
//     BEFORE a successful step-2 close while ownSessionExists is set
//     (today: RefuseQuiesce with ownSessionExists) — is the SAME teardown
//     class: the close never ran, so the session may be open ⇒ emit +
//     flush + TerminateProcess-self(19), never `delete lib`.
//   * every refuse AFTER a successful step-2 close (census/attach/
//     forceshutdown/verify-death) — or with no session ever existing:
//     exit 19 via normal teardown; `delete lib` is safe because the close
//     already succeeded (or never was needed).
//   * Recycled: exit 18 (caller-resume contract); same `delete lib`
//     post-close teardown.
//   * R1/R2 defer (recycleDeferredResult — an attached-site breach that
//     never ran the engine): R2 runs THIS §5 close stage first (the
//     shared boundedCloseWarehouse below) because only GRACEFUL
//     teardown — closeWarehouse + `delete lib` + ExitProcess —
//     releases the +7 (TerminateProcess orphans it for >=minutes,
//     live-proven 2026-10-21 — the corpse lock then refuses the
//     resumed process's census). A granted defer (close ok) exits 18
//     via the post-close teardown (`delete lib` + graceful ExitProcess
//     — the resume boundary, not a refusal); a close that fails or
//     outlives the bound REFUSES the defer on the close-stage family —
//     the own session may still be open, so emit + flush +
//     TerminateProcess-self(19), never `delete lib`/ExitProcess into a
//     possibly-open warehouse.
// R3 stages the exit codes and the tail-bypass flag contract; R4 owns the
// resume markers/manifest and the results-array boundary details.
//
// Pure Win32 (kernel32 + toolhelp + Ole32/OleAut32 for the production COM
// attach — the proven wh_shutdown.cpp chain). No orlib20, no warehouse,
// no diag dependency.

#ifndef TSDEV_WHSERVER_RECYCLE_H
#define TSDEV_WHSERVER_RECYCLE_H

#include "whserver_census.h"

#include <windows.h>
#include <ole2.h>

#include <functional>
#include <string>
#include <vector>

namespace tsdev {

// Engine outcome. Exit codes per design §6.2: 18 = whserver_recycled
// (caller resumes in a fresh process/broker), 19 = whserver_recycle_refused
// (a fail-closed stage could not confirm safety — nothing was recycled).
enum class RecycleOutcome {
    Recycled,              // verify-death passed (or server already absent)
    RefuseQuiesce,         // step 1: dispatch marker still in-flight
    RefuseCloseFailed,     // step 2: closeWarehouse raised SEH / returned
                           // false, closeBoundSecs<=0, or the pre-close
                           // zombie gate refused (the close never ran =>
                           // an own session may still be open)
    RefuseCensus,          // step 3: lock>0 at budget, Indeterminate read,
                           // or unverifiable evidence at budget/stream end
    RefuseAttach,          // step 4: MyWork unresolved / COM attach failed,
                           // or the pre-attach zombie gate refused
    RefuseForceShutDown,   // step 4: ForceShutDown invoke failed (non-death)
    RefuseVerifyDeath,     // step 5: forceshutdown_inert / zombie_present /
                           // death_unverifiable
};

// Per-invoke classification of the step-4 attach+ForceShutDown chain
// (design §2 step 4). ServerDied = the RPC failed in a way consistent with
// the server having died between census-pass and attach — treated as
// recycle-achieved-equivalent; the engine proceeds to step-5 verify-death.
enum class RecycleAttachStatus {
    Ok,
    AttachFailed,
    ForceShutDownFailed,
    ServerDied,
};

// Step-4 seam (R3 spec): production = raw COM replica of the proven
// tmp/wh_shutdown.cpp chain (ComRecycleAttach below); tests inject fakes.
// attachAndForceShutDown runs the attach+invoke sequence once per pid in
// censusPids (§2 step-4 multi-pid contract: class-to-pid binding is
// ambiguous under >1 pid but MOOT — verify-death requires EVERY enumerated
// pid gone). The engine passes only the pids still listed in the
// Pass-completing re-enumeration: pids that already reached re-enumerated
// absence are dead (verify-death still confirms the FULL union) and their
// images can no longer be verified anyway (P4 f-i). release() drops any
// held COM refs (§2 step 6; RPC errors on a dead server tolerated, never
// reported).
class IRecycleAttach {
public:
    virtual ~IRecycleAttach() {}
    virtual RecycleAttachStatus attachAndForceShutDown(
        const std::vector<DWORD>& censusPids, std::string& detail) = 0;
    virtual void release() = 0;
};

// One step-5 verify-death poll (design §2 step 5): a fresh WHServer
// re-enumeration + the TSCrashReport parent-pid set + which census pids are
// confirmed still alive (OpenProcess ok && STILL_ACTIVE, or OpenProcess
// denied — denied is never evidence of death, it is a fail-closed recheck).
struct RecycleDeathPoll {
    bool               snapshotOk = false;      // false = WHServer enumeration
                                                // failed/inconclusive
    bool               crashSnapshotOk = false; // false = TSCrashReport
                                                // enumeration inconclusive —
                                                // NEVER "no zombies"
    std::vector<DWORD> whPids;              // fresh WHServer pid set
    std::vector<DWORD> crashParentPids;     // parents of TSCrashReport.exe
    std::vector<DWORD> stillActive;         // census pids confirmed alive/denied
};

// One zombie-gate poll (the P4-(b) corpse pre-check the engine runs ahead
// of step 2 and step 4): a fresh WHServer re-enumeration + the
// TSCrashReport parent-pid set. A pid present in BOTH is a crash-frozen
// corpse — it still enumerates and reads lock==0 while wedging every
// dispatch toward it (closeWarehouse / the COM attach would hang). Either
// flag false => the check is inconclusive — NEVER "no zombies".
struct RecycleZombiePoll {
    bool               snapshotOk = false;      // false = WHServer
                                                // enumeration failed
    bool               crashSnapshotOk = false; // false = TSCrashReport
                                                // enumeration inconclusive
    std::vector<DWORD> whPids;              // fresh WHServer pid set
    std::vector<DWORD> crashParentPids;     // parents of TSCrashReport.exe
};

// Impure-seam for the engine's process reads and clock (§3.4 discipline):
// production = WhRecycleSampler (real findWhServerPids + readWhLockSample +
// findCrashReportParentPids + GetTickCount64 + Sleep); tests script streams.
class IRecycleSampler {
public:
    virtual ~IRecycleSampler() {}
    // One census poll: fresh re-enumeration (snapshotOk=false on snapshot
    // failure — NEVER evidence of absence) + per-listed-pid lock read. The
    // impl fills poll.timeSecs itself. Returns false when the stream is
    // exhausted (test streams; production never exhausts).
    virtual bool censusPoll(CensusPoll& out) = 0;
    // One zombie-gate poll against the CURRENT WHServer set — the corpse
    // pre-check run ahead of step 2 (a crash-frozen server would wedge
    // closeWarehouse) and step 4 (it would wedge the COM attach into the
    // watchdog kill). A crash-report parent pid intersecting the fresh
    // WHServer set marks a zombie. Returns false when the stream is
    // exhausted (test streams; production never exhausts).
    virtual bool zombiePoll(RecycleZombiePoll& out) = 0;
    // One verify-death poll against the census-time pid set.
    virtual bool deathPoll(const std::vector<DWORD>& censusPids,
                           RecycleDeathPoll& out) = 0;
    virtual double nowSecs() = 0;       // monotonic seconds
    virtual void   sleepMs(unsigned ms) = 0;
};

// Engine parameters (production values are the §3.3/§2-step-5 constants).
struct RecycleParams {
    bool dispatchInFlight = false;        // step-1 assert: dispatchSnapshot().inFlight
    bool ownSessionExists = false;        // step-2 conditional (§2/F4)
    // Called ONLY when ownSessionExists; production binds
    // lib->closeWarehouse(). Must NOT be invoked on a never-attached lib —
    // its never-opened behavior is UNTESTED (design §2 step 2).
    std::function<bool()> closeSession;
    // Step-2 bound-exceeded action — invoked on the monitor thread when
    // closeBoundSecs elapses with the close still in flight. EMPTY in
    // production: the engine installs the default (stderr +
    // TerminateProcess(19) — a timed-out close leaves the engine thread
    // wedged inside a possibly-open-session call that can never return a
    // refuse, so the monitor applies the §5 close_failed teardown itself,
    // the dispatch-watchdog fire() convention). Tests inject a stub that
    // records the fire and unblocks the scripted close; the action must
    // never wait on the engine thread's progress.
    std::function<void()> onCloseBoundExceeded;
    IRecycleSampler* sampler = nullptr;
    IRecycleAttach*  attach  = nullptr;
    double closeBoundSecs    = 30.0;  // step-2 monitor bound: a legit close
                                      // is a short dispatch — far above it,
                                      // far below the 300 s dispatch-
                                      // watchdog backstop
    double pollIntervalSecs  = 0.2;   // ~200 ms (within the 150-400 ms band)
    double censusWindowSecs  = 3.0;   // sustained-zero window (PROVEN gate)
    double censusBudgetSecs  = 30.0;  // census bound (§3.3 design choice)
    double verifyBoundSecs   = 15.0;  // ~2.5x the measured ~6 s shutdown
    unsigned deathConfirmPolls = 3;   // mirrors kWhServerGoneConfirmSamples
};

struct RecycleResult {
    RecycleOutcome outcome = RecycleOutcome::RefuseCensus;
    int            exitCode = 19;        // 18 recycled / 19 refused
    // §5 teardown contract (see header top): close_failed, or any refuse
    // taken before a successful step-2 close while a session exists =>
    // emit + flush + TerminateProcess-self(19); never `delete lib`.
    bool           terminateSelf = false;
    // `delete lib` is safe: step-2 close succeeded, or no session existed.
    bool           safeToDeleteLib = false;
    // Reason classification for the refuse event/response:
    // "quiesce_in_flight" | "close_failed" | "close_timeout" |
    // "close_bound_invalid" | "zombie_present_close" |
    // "zombie_unverifiable_close" | "census_lock" | "census_unverifiable" |
    // "census_indeterminate" | "zombie_present_attach" |
    // "zombie_unverifiable_attach" | "attach_failed" |
    // "forceshutdown_failed" | "forceshutdown_inert" | "zombie_present" |
    // "death_unverifiable" ("whserver_recycled" on Ok).
    std::string    reason;
    std::string    detail;               // pid / GetLastError / HRESULT context
};

// The fail-closed engine: runs §2 steps 1-6 in order on the caller's thread
// (the batch/serve worker STA). Returns the outcome; ALL process exit and
// emission happen at the call site (R4 owns the §4.1 emission ordering —
// R3 wires the exit codes + the terminateSelf/safeToDeleteLib contract).
RecycleResult runWhserverRecycle(const RecycleParams& p);

// --- The shared §5 close stage (R2) -------------------------------------------
//
// Zombie pre-check + monitor-bounded closeWarehouse, run on the CALLING
// thread — the warehouse-OWNING thread (orlib20 binds CELWHouse per
// (thread,path) — the export is literally
// _CloseWarehouseForCurrentThreadAndPath@4 — so a close dispatched on any
// other thread looks up THAT thread's binding, finds none, and no-op
// succeeds — a false-pass that leaves the real session open). The blocked
// call therefore stays on the owning thread and the timed wait moves to a
// monitor thread — the same inversion the engine's step-2 uses (a wedged
// call cannot be cancelled either way).
//
//   sampler          -> the P4-(b) pre-close zombie gate: a crash-frozen
//                       WHServer still enumerates and wedges the close
//                       into the bound. nullptr counts as an INCONCLUSIVE
//                       gate — never "no zombies" (fail-closed).
//   boundSecs        -> the monitor bound in seconds (the engine's
//                       RecycleParams::closeBoundSecs, 30 s production);
//                       <=0 is not a usable bound.
//   onBoundExceeded  -> action the monitor runs when the bound expires
//                       with the close still in flight. EMPTY installs
//                       the default: stderr + TerminateProcess(19) — a
//                       wedged close can never return a refuse, so the
//                       monitor applies the §5 teardown itself (the
//                       dispatch_watchdog.cpp fire() convention). A
//                       non-fatal injected action (tests) yields the
//                       "close_timeout" refusal — the late close return
//                       is never trusted.
//
// Returns true ONLY when the close ran AND returned true inside the
// bound. On false, refuseReason/refuseDetail (both optional out-params)
// carry the §5 close-stage refuse labels and diagnostics:
// "zombie_present_close" / "zombie_unverifiable_close" (the gate),
// "close_bound_invalid", "close_timeout", "close_failed" — every one
// means the own session may still be open (the §5 terminateSelf
// teardown class applies: emit + flush + TerminateProcess-self, never
// `delete lib`/ExitProcess into a possibly-open warehouse).
bool boundedCloseWarehouse(
    const std::function<bool()>& closeSession,
    IRecycleSampler* sampler,
    double boundSecs,
    const std::function<void()>& onBoundExceeded = nullptr,
    std::string* refuseReason = nullptr,
    std::string* refuseDetail = nullptr);

// --- Production seams --------------------------------------------------------

// Real sampler: Toolhelp re-enumeration + readWhLockSample at the fixed
// WHServer VAs + findCrashReportParentPids (step-5 zombie check and the
// step-2/step-4 zombie pre-checks) + OpenProcess/STILL_ACTIVE for the
// still-active set. Read-only; zero effect on the server.
class WhRecycleSampler : public IRecycleSampler {
public:
    bool censusPoll(CensusPoll& out) override;
    bool zombiePoll(RecycleZombiePoll& out) override;
    bool deathPoll(const std::vector<DWORD>& censusPids,
                   RecycleDeathPoll& out) override;
    double nowSecs() override;
    void   sleepMs(unsigned ms) override;
};

// Production step-4 attach: the proven tmp/wh_shutdown.cpp raw-COM chain
// (CoGetClassObject CLSID_Control -> factory vtbl+0x20 create -> IDispatch
// -> WHouseEx(Create=1, MyWork) -> ForceShutDown), run once per census pid.
// `myWorkDir` is a HARD precondition: empty/unresolved => every invoke
// returns AttachFailed WITHOUT touching COM (§2 step 4: NEVER invoke
// WHouseEx Create=1 against an unresolved path).
// Build-identity gate: EVERY census pid is verified against the probed
// WHServer.exe build (whserver_census.h SSOT) before any COM call — the
// chain's vtable offsets/dispids are layout facts of that one build. The
// production census sampler gates the same check upstream; attach repeats
// it so a direct caller is fail-closed even when the census never ran.
// INFERRED/UNTESTED (design §2 step 4): the proven chain ran in a FRESH
// process; attaching on the same STA AFTER closeWarehouse tore down
// CELWHouse is the re-activation-shaped posture the 0x4E hazard warns
// about — covered only by the deferred closing-gate live E2E.
class ComRecycleAttach : public IRecycleAttach {
public:
    explicit ComRecycleAttach(const std::wstring& myWorkDir);
    ~ComRecycleAttach() override { release(); }
    RecycleAttachStatus attachAndForceShutDown(
        const std::vector<DWORD>& censusPids, std::string& detail) override;
    void release() override;
private:
    std::wstring m_myWorkDir;
    IUnknown*    m_fact   = nullptr;
    IUnknown*    m_ctl    = nullptr;
    IDispatch*   m_dctrl  = nullptr;
    IDispatch*   m_whouse = nullptr;
    BSTR         m_outName = nullptr;
    VARIANT      m_vres{};   // VariantInit'ed in the ctor, cleared in release()
};

// Derive the MyWork directory (wide) from TsDevLib's getMyWorkBinPath()
// ("<root>\MyWork\Bin" -> "<root>\MyWork"). Returns empty when the input is
// empty or has no parent component — callers pass the empty result into
// ComRecycleAttach, which then refuses attach_failed without touching COM.
std::wstring resolveMyWorkDirFromBinPath(const char* myWorkBinPath);

} // namespace tsdev

#endif // TSDEV_WHSERVER_RECYCLE_H
