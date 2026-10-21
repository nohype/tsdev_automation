// whserver_recycle.cpp
//
// Implementation of the WHServer gated auto-recycle engine declared in
// whserver_recycle.h — tmp/RECYCLE_ENGINE_DESIGN.md §2 steps 1-6, R3
// "engine skeleton". The fail-closed contract lives in the header; this
// file is the sequencing + the two production seams (WhRecycleSampler and
// the ComRecycleAttach raw-COM replica of tmp/wh_shutdown.cpp).

#include "whserver_recycle.h"

#include "dispatch_watchdog.h"   // DispatchInFlightGuard / findCrashReportParentPids
#include "whserver_health.h"     // findWhServerPids()

#include <algorithm>
#include <cstdlib>   // _lrotr
#include <cstdio>
#include <process.h>   // _beginthreadex (step-2 bounded close monitor)

namespace tsdev {

namespace {

constexpr int kExitRecycled = 18;
constexpr int kExitRefused  = 19;

bool containsPid(const std::vector<DWORD>& v, DWORD pid) {
    return std::find(v.begin(), v.end(), pid) != v.end();
}

RecycleResult refuse(RecycleOutcome o, const char* reason,
                     const std::string& detail, bool safeToDeleteLib,
                     bool terminateSelf) {
    RecycleResult r;
    r.outcome          = o;
    r.exitCode         = kExitRefused;
    r.terminateSelf    = terminateSelf;
    r.safeToDeleteLib  = safeToDeleteLib;
    r.reason           = reason;
    r.detail           = detail;
    return r;
}

std::string censusDetail(const CensusDecision& d) {
    std::string s;
    if (d.pid != 0) {
        s += "pid=" + std::to_string(static_cast<unsigned long>(d.pid));
    }
    if (d.error != 0) {
        if (!s.empty()) s += " ";
        s += "err=" + std::to_string(static_cast<unsigned long>(d.error));
    }
    return s;
}

// --- P4 helpers ---------------------------------------------------------------

// A census Refuse is honestly "census_lock" ONLY when a listed pid was
// positively OBSERVED holding lock>0 (foreign client-held objects — the
// gate's own §3.3 meaning). A Refuse reached on missing or inconclusive
// evidence — re-enumeration failures, never-read pids, an unmet
// sustained-zero window, an exhausted stream — is "census_unverifiable":
// the gate could not confirm quiescence but never measured a held lock
// either (P4 f-iv). Any observed lock>0 anywhere in the stream wins the
// lock label: it is affirmative evidence even when the final polls went
// inconclusive. The valid-read predicate mirrors decideCensus' own
// discipline (attempted + imageKnown + both errors 0 + still listed).
bool censusObservedLock(const std::vector<CensusPoll>& polls) {
    for (const CensusPoll& poll : polls) {
        if (!poll.snapshotOk) continue;
        for (const CensusPidRead& r : poll.reads) {
            if (r.attempted && r.imageKnown &&
                containsPid(poll.pids, r.pid) &&
                r.sample.openError == 0 && r.sample.rpmError == 0 &&
                r.sample.lockValue != 0) {
                return true;
            }
        }
    }
    return false;
}

// The P4-(b) corpse pre-check the engine runs ahead of step 2 (a
// crash-frozen server would wedge closeWarehouse into the close bound)
// and ahead of step 4 (the same corpse passes the census — its frozen
// memory still reads lock==0 — then wedges the COM attach into the
// dispatch-watchdog kill). A TSCrashReport.exe parent pid intersecting
// the fresh WHServer set marks a zombie. `stage` tags the refuse reason
// ("close" | "attach"). Returns true when the gate REFUSES — either a
// positively detected zombie ("zombie_present_<stage>") or an
// inconclusive check ("zombie_unverifiable_<stage>": a failed
// re-enumeration is never "no zombies", and unknown zombie state is
// never proceeded on). On false both enumerations were conclusive and
// no currently-enumerated WHServer parents a crash reporter.
bool zombieGateFails(IRecycleSampler* s, const char* stage,
                     std::string& reason, std::string& detail) {
    RecycleZombiePoll zp;
    const bool polled = s->zombiePoll(zp);
    if (!polled || !zp.snapshotOk || !zp.crashSnapshotOk) {
        reason = std::string("zombie_unverifiable_") + stage;
        detail = !polled        ? "zombie_poll_unavailable"
                 : !zp.snapshotOk ? "whserver_enum_inconclusive"
                                  : "crashreport_enum_inconclusive";
        return true;
    }
    for (DWORD parent : zp.crashParentPids) {
        if (containsPid(zp.whPids, parent)) {
            reason = std::string("zombie_present_") + stage;
            detail = "pid=" +
                std::to_string(static_cast<unsigned long>(parent));
            return true;
        }
    }
    return false;
}

// --- Step-2 bounded close monitor -------------------------------------------
// orlib20 binds CELWHouse per (thread, path) — the close export is
// literally _CloseWarehouseForCurrentThreadAndPath@4 and activation is once
// per (thread,path) for process life (KB: Ghidra-proven) — so closeWarehouse
// MUST run on the engine's own (warehouse-owning) thread: a helper-thread
// call would look up ITS thread's binding, find none, and return a no-op
// success — a false-pass that leaves the real session open and lets a
// post-"close" refuse `delete lib` into it (the exact modal-detach hang
// this bound exists to prevent). The bound therefore inverts
// runBoundedControlRead's arrangement (tsdev_cli.cpp): the blocked call
// stays on the engine thread and the timed wait moves to a monitor
// thread — a wedged call cannot be cancelled either way.

// Bound-exceeded action default: the §5 close_failed teardown applied
// directly — a timed-out close leaves the engine thread inside a
// possibly-open-session call that can never produce a RecycleResult.
// stderr only (the emitLine channel is a CLI-main-thread lambda and is not
// safe to invoke from another thread — the dispatch_watchdog.cpp fire()
// convention), then TerminateProcess(19), NOT ExitProcess/`delete lib`
// into a possibly-open warehouse.
[[noreturn]] void closeBoundAbortFire(DWORD boundMs) {
    std::fprintf(stderr,
                 "tsdev_cli: RECYCLE ABORT: closeWarehouse exceeded the "
                 "%lu ms close bound; the session may still be open; "
                 "exiting with code %d\n",
                 static_cast<unsigned long>(boundMs), kExitRefused);
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), kExitRefused);
    // TerminateProcess does not return; unreachable.
    std::abort();
}

// Monitor job on the engine's stack; the engine joins the monitor before
// leaving the step-2 scope, so every reference stays valid for the
// monitor's whole life.
struct CloseBoundMonitor {
    HANDLE                       done;       // set on close return
    DWORD                        timeoutMs;
    const std::function<void()>* onTimeout;  // never null at thread start
    bool                         fired;      // monitor-written before onTimeout
};

unsigned __stdcall closeBoundMonitorProc(void* arg) {
    CloseBoundMonitor* m = static_cast<CloseBoundMonitor*>(arg);
    if (WaitForSingleObject(m->done, m->timeoutMs) == WAIT_OBJECT_0) {
        return 0;  // close returned inside the bound
    }
    // Re-check once: a completion racing the boundary can land between the
    // wait's expiry and here; honoring it keeps a completed close from
    // false-firing the abort.
    if (WaitForSingleObject(m->done, 0) == WAIT_OBJECT_0) return 0;
    // TIMEOUT (or a failed wait — completion is unconfirmable either way).
    // Flag first so a non-fatal injected action still lets the engine
    // classify the bound, then run the action.
    m->fired = true;
    (*m->onTimeout)();
    return 0;
}

} // namespace

// --- The shared §5 close stage (R2) --------------------------------------------
//
// The engine's step-2 machinery lifted to a shared entry point: the R2
// attached-site defer runs the SAME stage (zombie pre-check + bounded
// close on the calling/session-owning thread) before its graceful
// exit-18. Ordering mirrors the engine verbatim: params -> zombie gate
// -> empty-seam -> arm monitor -> close -> bound-fired / close-false.

bool boundedCloseWarehouse(
    const std::function<bool()>& closeSession,
    IRecycleSampler* sampler,
    double boundSecs,
    const std::function<void()>& onBoundExceeded,
    std::string* refuseReason,
    std::string* refuseDetail) {
    auto refuseOut = [&](const char* reason,
                         const std::string& detail) -> bool {
        if (refuseReason) *refuseReason = reason;
        if (refuseDetail) *refuseDetail = detail;
        return false;
    };

    // A non-positive bound is not a bound at all — 0 fires the monitor
    // instantly (refusing every legit close) and a negative wraps the
    // DWORD millisecond conversion to ~49.7 days (a silently unbounded
    // close). Same fail-closed discipline as the engine's entry guard.
    if (boundSecs <= 0.0) {
        return refuseOut("close_bound_invalid",
                         "closeBoundSecs<=0 is not a usable close bound");
    }

    // P4-(b) corpse pre-check BEFORE the close: a crash-frozen WHServer
    // still enumerates (a frozen corpse is not absent), so the VAS
    // guard can trip on it — and closeWarehouse would then wedge inside
    // the dispatch, discovered only when the close bound kills the
    // process 30 s later. Refuse early with the honest stage-tagged
    // reason; the close never ran, so an own session may still be open
    // => the §5 close-stage teardown class. A missing sampler is
    // inconclusive the same way (unknown zombie state is never
    // proceeded on).
    if (sampler == nullptr) {
        return refuseOut("zombie_unverifiable_close",
                         "zombie_poll_unavailable");
    }
    {
        std::string zReason, zDetail;
        if (zombieGateFails(sampler, "close", zReason, zDetail)) {
            return refuseOut(zReason.c_str(), zDetail);
        }
    }

    if (!closeSession) {
        return refuseOut("close_failed",
                         "closeSession seam empty; session may still "
                         "be open");
    }

    // The bound is the monitor thread armed before the call: a legit
    // close is a short dispatch, so a boundSecs overrun means a modal
    // (non-dispatch) hang. The empty/default action is stderr +
    // TerminateProcess(19) — a wedged call can never return, so the
    // monitor applies the §5 teardown itself; a non-fatal injected
    // action lets the caller classify "close_timeout" — the late
    // return is never trusted.
    const DWORD boundMs = static_cast<DWORD>(boundSecs * 1000.0);
    std::function<void()> timeoutAction = onBoundExceeded;
    if (!timeoutAction) {
        timeoutAction = [boundMs] { closeBoundAbortFire(boundMs); };
    }
    CloseBoundMonitor mon;
    mon.done      = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    mon.timeoutMs = boundMs;
    mon.onTimeout = &timeoutAction;
    mon.fired     = false;
    const uintptr_t th =
        (mon.done != nullptr)
            ? _beginthreadex(nullptr, 0, closeBoundMonitorProc, &mon, 0,
                             nullptr)
            : 0;
    if (mon.done == nullptr || th == 0) {
        // Cannot arm the bound — running the close unmonitored would
        // reopen the unbounded window; refuse on the same teardown
        // contract (the close never ran, so the session may be open).
        if (mon.done != nullptr) CloseHandle(mon.done);
        if (th != 0) CloseHandle(reinterpret_cast<HANDLE>(th));
        return refuseOut("close_failed",
                         "could not arm the bounded close monitor");
    }
    const bool closed = closeSession();
    SetEvent(mon.done);
    WaitForSingleObject(reinterpret_cast<HANDLE>(th), INFINITE);
    CloseHandle(reinterpret_cast<HANDLE>(th));
    CloseHandle(mon.done);
    if (mon.fired) {
        return refuseOut("close_timeout",
                         "closeWarehouse exceeded the close bound; "
                         "session may still be open");
    }
    if (!closed) {
        return refuseOut("close_failed",
                         "closeWarehouse failed; session may still be "
                         "open");
    }
    return true;
}

// --- The engine (§2 steps 1-6) ------------------------------------------------

RecycleResult runWhserverRecycle(const RecycleParams& p) {
    // Params validation at engine entry (P4 f-ii): a non-positive close
    // bound is not a bound at all — 0 fires the monitor instantly
    // (refusing every legit close) and a negative wraps the DWORD
    // millisecond conversion to ~49.7 days (a silently unbounded close).
    // Same fail-closed discipline as decideCensus' windowSecs<=0 Refuse:
    // invalid gate params refuse BEFORE anything runs. With an own
    // session the close can never run on this path, so the
    // session-may-be-open teardown class applies (§5 rows 1-2).
    if (p.closeBoundSecs <= 0.0) {
        return refuse(RecycleOutcome::RefuseCloseFailed,
                      "close_bound_invalid",
                      "closeBoundSecs<=0 is not a usable close bound",
                      /*safeToDeleteLib=*/!p.ownSessionExists,
                      /*terminateSelf=*/p.ownSessionExists);
    }

    // Same params-invalid guard for the poll interval (P4 nit, identical
    // wrap class): a negative pollIntervalSecs wraps the
    // static_cast<unsigned>(secs * 1000.0) conversion at every
    // census/verify-death sleepMs to ~49.7 days (a silently stalled
    // engine), and 0 is not a usable interval — the polls would spin
    // back-to-back with no settle time. Refuse at engine entry on the
    // identical teardown contract: nothing ran, so with an own session
    // the close can never run on this path and the session-may-be-open
    // teardown class applies (§5 rows 1-2).
    if (p.pollIntervalSecs <= 0.0) {
        return refuse(RecycleOutcome::RefuseCloseFailed,
                      "poll_interval_invalid",
                      "pollIntervalSecs<=0 is not a usable poll interval",
                      /*safeToDeleteLib=*/!p.ownSessionExists,
                      /*terminateSelf=*/p.ownSessionExists);
    }

    // Step 1 — quiesce (§2): at every trigger site there is provably no
    // in-flight op; the caller asserted it via dispatchSnapshot. If the
    // marker is somehow in-flight, refuse. With a live own session the
    // teardown state is the same "possibly-open warehouse" uncertainty as
    // close_failed — §5: TerminateProcess-self, never `delete lib`.
    if (p.dispatchInFlight) {
        return refuse(RecycleOutcome::RefuseQuiesce, "quiesce_in_flight",
                      "dispatch marker in-flight at recycle trigger",
                      /*safeToDeleteLib=*/!p.ownSessionExists,
                      /*terminateSelf=*/p.ownSessionExists);
    }

    // Step 2 — closeWarehouse() bounded-wait, CONDITIONAL on an own session
    // existing (§2/F4): skipped entirely at never-attached sites — calling
    // closeWarehouse on a never-opened TsDevLib is UNTESTED, not a proven
    // no-op. The whole stage (P4-(b) zombie pre-check -> monitor-bounded
    // close on THIS thread) lives in the shared boundedCloseWarehouse —
    // the SAME stage the R2 attached-site defer runs before its graceful
    // exit-18. Any refuse here means the close never ran (or its return
    // is untrusted), so the session may still be OPEN => emit+flush+
    // TerminateProcess-self(19), never `delete lib`/ExitProcess into a
    // possibly-open warehouse (§5 rows 1-2, dispatch_watchdog.cpp:79-88
    // convention). A bound-expired close is the same teardown applied by
    // the monitor itself (the engine thread is the wedged one); under a
    // non-fatal injected action (tests) the helper reports close_timeout
    // on the same contract — the late return is untrusted.
    if (p.ownSessionExists) {
        std::string cReason, cDetail;
        if (!boundedCloseWarehouse(p.closeSession, p.sampler,
                                   p.closeBoundSecs,
                                   p.onCloseBoundExceeded,
                                   &cReason, &cDetail)) {
            return refuse(RecycleOutcome::RefuseCloseFailed,
                          cReason.c_str(), cDetail,
                          /*safeToDeleteLib=*/false,
                          /*terminateSelf=*/true);
        }
    }

    // Step 3 — census (§3): sustained-zero gate over a per-poll
    // RE-ENUMERATED pid set. Pass => every enumerated pid lock==0 for
    // >=censusWindowSecs (or confirmed absent). Refuse/Indeterminate =>
    // exit 19 via normal teardown — step-2 close succeeded (or no session
    // ever existed), so `delete lib` runs the same post-close path a
    // normal batch end runs.
    std::vector<CensusPoll> polls;
    std::vector<DWORD> censusPids;  // union of every enumerated pid
    CensusDecision dec{CensusVerdict::Refuse, 0, 0};
    for (;;) {
        CensusPoll poll{};
        if (!p.sampler->censusPoll(poll)) break;  // stream exhausted
        if (poll.snapshotOk) {
            for (DWORD pid : poll.pids) {
                if (!containsPid(censusPids, pid)) censusPids.push_back(pid);
            }
        }
        polls.push_back(poll);
        dec = decideCensus(polls, p.censusWindowSecs, p.censusBudgetSecs);
        if (dec.verdict == CensusVerdict::Pass) break;
        if (dec.verdict == CensusVerdict::Indeterminate) {
            return refuse(RecycleOutcome::RefuseCensus,
                          "census_indeterminate", censusDetail(dec),
                          /*safeToDeleteLib=*/true, /*terminateSelf=*/false);
        }
        // Refuse so far: inconclusive until the budget elapses — lock>0 (or
        // unverifiable state) at budget end means foreign client-held
        // objects (§3.3). Terminate the loop ourselves on the same bound.
        // The label distinguishes a positively-observed held lock from a
        // census that never produced one (P4 f-iv).
        if (polls.back().timeSecs - polls.front().timeSecs >=
            p.censusBudgetSecs) {
            return refuse(RecycleOutcome::RefuseCensus,
                          censusObservedLock(polls) ? "census_lock"
                                                    : "census_unverifiable",
                          censusDetail(dec),
                          /*safeToDeleteLib=*/true, /*terminateSelf=*/false);
        }
        p.sampler->sleepMs(
            static_cast<unsigned>(p.pollIntervalSecs * 1000.0));
    }
    if (dec.verdict != CensusVerdict::Pass) {
        // Stream exhausted (or never produced a poll) without a verdict:
        // inconclusive => fail closed (§3.4: inconclusive streams fail
        // closed). A Refuse labels "census_lock" ONLY when a held lock
        // was positively observed (P4 f-iv).
        return refuse(RecycleOutcome::RefuseCensus,
                      dec.verdict == CensusVerdict::Indeterminate
                          ? "census_indeterminate"
                          : censusObservedLock(polls)
                                ? "census_lock" : "census_unverifiable",
                      censusDetail(dec),
                      /*safeToDeleteLib=*/true, /*terminateSelf=*/false);
    }

    // Census passed with no pids ever enumerated (re-enumerated absence >=3
    // consecutive polls): the server is already gone — recycle-achieved by
    // definition; skip attach and verify-death entirely (§3.3 absent arm).
    if (censusPids.empty()) {
        RecycleResult r;
        r.outcome         = RecycleOutcome::Recycled;
        r.exitCode        = kExitRecycled;
        r.safeToDeleteLib = true;
        r.reason          = "whserver_recycled";
        r.detail          = "server absent at census";
        return r;
    }

    // Died-mid-census sub-case (§3.3 absent arm): a non-empty pid union can
    // still pass via absence alone — the Pass-completing poll (polls.back(),
    // guaranteed snapshotOk: Pass is only granted on a successful
    // re-enumeration) lists NONE of the union pids, i.e. every one reached
    // >=3 consecutive re-enumerated absences. Attaching now would let
    // CoGetClassObject spawn a FRESH WHServer the gate never measured and
    // ForceShutDown it — the exact hazard the census exists to prevent.
    // Re-enumerated absence >=3 consecutive polls is the same evidence
    // step-5 verify-death requires, so this is recycle-achieved: skip
    // attach AND verify (§2 step 4 server-died equivalence).
    //
    // attachPids (P4 f-i): only the pids still listed in that final
    // re-enumeration may be attached. A pid already re-enumeration-absent
    // is dead — verify-death still confirms the FULL union — and its
    // image can no longer be verified anyway (OpenProcess on a dead pid
    // fails): gating/invoking on the union would refuse a legitimate
    // recycle of the live pids — the mixed-Pass over-refusal.
    std::vector<DWORD> attachPids;
    for (DWORD pid : censusPids) {
        if (containsPid(polls.back().pids, pid)) attachPids.push_back(pid);
    }
    if (attachPids.empty()) {
        RecycleResult r;
        r.outcome         = RecycleOutcome::Recycled;
        r.exitCode        = kExitRecycled;
        r.safeToDeleteLib = true;
        r.reason          = "whserver_recycled";
        r.detail          = "server died mid-census (re-enumerated absence)";
        return r;
    }

    // Step 4 — attach + ForceShutDown (INFERRED/UNTESTED for the
    // same-STA-post-close posture — see whserver_recycle.h). A call failure
    // classified as server death skips to step 5 (a natural self-reap is an
    // equivalent outcome); any other failure releases our refs and refuses.
    //
    // P4-(b) corpse pre-check BEFORE attach: a crash-frozen server passes
    // the census (its frozen memory still reads lock==0), so only this
    // gate stands between the engine and a COM attach that wedges into
    // the dispatch-watchdog kill. Earlier and better-labeled than the
    // hang; the step-2-close stage is already behind us => normal
    // teardown. No release() yet — no COM refs were ever taken.
    std::string attachDetail;
    {
        std::string zReason, zDetail;
        if (zombieGateFails(p.sampler, "attach", zReason, zDetail)) {
            return refuse(RecycleOutcome::RefuseAttach,
                          zReason.c_str(), zDetail,
                          /*safeToDeleteLib=*/true,
                          /*terminateSelf=*/false);
        }
    }
    const RecycleAttachStatus attachStatus =
        p.attach->attachAndForceShutDown(attachPids, attachDetail);
    if (attachStatus == RecycleAttachStatus::AttachFailed) {
        p.attach->release();
        return refuse(RecycleOutcome::RefuseAttach, "attach_failed",
                      attachDetail,
                      /*safeToDeleteLib=*/true, /*terminateSelf=*/false);
    }
    if (attachStatus == RecycleAttachStatus::ForceShutDownFailed) {
        p.attach->release();
        return refuse(RecycleOutcome::RefuseForceShutDown,
                      "forceshutdown_failed", attachDetail,
                      /*safeToDeleteLib=*/true, /*terminateSelf=*/false);
    }
    // Ok or ServerDied -> verify death either way.

    // Step 5 — verify FULL process death (bounded): EVERY census-enumerated
    // pid absent from >=deathConfirmPolls consecutive successful
    // re-enumerations, no TSCrashReport child (a frozen zombie still
    // enumerates and gets reactivated), and no census pid confirmed
    // still-alive. A failed WHServer OR crash-report re-enumeration is
    // inconclusive, never absent (snapshot-failure ambiguity is PROVEN).
    // Timeout => refuse; we never
    // escalate to taskkill (mandate §0) — we report and refuse.
    // The refuse label distinguishes a POSITIVELY-OBSERVED survivor
    // ("forceshutdown_inert"/"zombie_present" — a conclusive poll saw the
    // pid alive/frozen) from a bound or stream that never produced
    // conclusive evidence either way ("death_unverifiable", P4 f-iv).
    unsigned deadStreak = 0;
    const double vStart = p.sampler->nowSecs();
    bool verified = false;
    bool aliveObserved = false;   // a conclusive poll PROVED a census pid
                                  // alive/zombie — the only evidence that
                                  // may name the refuse inert/zombie
    const char* lastWhy = "death_unverifiable";
    std::string lastDetail;
    while (p.sampler->nowSecs() - vStart <= p.verifyBoundSecs) {
        RecycleDeathPoll dp;
        if (!p.sampler->deathPoll(censusPids, dp)) break;  // stream exhausted
        if (!dp.snapshotOk || !dp.crashSnapshotOk) {
            deadStreak = 0;  // either enumeration inconclusive — never
                             // counts toward absence
            if (!aliveObserved) {
                lastDetail = !dp.snapshotOk
                                 ? "whserver_enum_inconclusive"
                                 : "crashreport_enum_inconclusive";
            }
        } else {
            bool zombie = false;
            bool inert  = false;
            for (DWORD pid : censusPids) {
                if (containsPid(dp.crashParentPids, pid)) zombie = true;
                if (containsPid(dp.whPids, pid) ||
                    containsPid(dp.stillActive, pid)) {
                    inert = true;
                }
                if (zombie || inert) {
                    lastDetail = "pid=" +
                        std::to_string(static_cast<unsigned long>(pid));
                }
            }
            if (!zombie && !inert) {
                if (++deadStreak >= p.deathConfirmPolls) {
                    verified = true;
                    break;
                }
                if (!aliveObserved) {
                    lastDetail = "absence_unconfirmed streak=" +
                        std::to_string(static_cast<unsigned long>(deadStreak));
                }
            } else {
                deadStreak = 0;
                aliveObserved = true;
                lastWhy = zombie ? "zombie_present" : "forceshutdown_inert";
            }
        }
        p.sampler->sleepMs(
            static_cast<unsigned>(p.pollIntervalSecs * 1000.0));
    }

    // Step 6 — release our recycle refs (RPC errors on a dead server
    // tolerated inside release(); CoUninitialize is NOT called — the
    // apartment belongs to TsDevLib's guard).
    p.attach->release();

    if (!verified) {
        return refuse(RecycleOutcome::RefuseVerifyDeath, lastWhy, lastDetail,
                      /*safeToDeleteLib=*/true, /*terminateSelf=*/false);
    }

    RecycleResult r;
    r.outcome         = RecycleOutcome::Recycled;
    r.exitCode        = kExitRecycled;
    r.safeToDeleteLib = true;
    r.reason          = "whserver_recycled";
    return r;
}

// --- WhRecycleSampler (production impure seam) ---------------------------------

bool WhRecycleSampler::censusPoll(CensusPoll& out) {
    out = CensusPoll{};
    out.timeSecs = nowSecs();
    std::vector<DWORD> pids;
    out.snapshotOk = findWhServerPids(pids);  // snapshotOk overload: false
                                            // only on snapshot failure
    if (!out.snapshotOk) return true;
    out.pids = pids;
    for (DWORD pid : pids) {
        CensusPidRead r;
        r.pid = pid;
        r.attempted = true;
        // Image gate first: the fixed VAs are layout facts of ONE probed
        // WHServer build. On an unprobed image the same VA can still map
        // and read a persistent 0 — the stale-VA false-pass — so the RPM
        // is skipped entirely and decideCensus goes Indeterminate.
        r.imageKnown = whServerPidImageIsKnown(pid);
        r.sample = WhLockSample{};
        if (r.imageKnown) {
            const CensusTarget t{pid, kWhLockVa, kWhActivityVa};
            readWhLockSample(t, r.sample);  // false leaves error fields set
        }
        out.reads.push_back(r);
    }
    return true;
}

bool WhRecycleSampler::deathPoll(const std::vector<DWORD>& censusPids,
                                 RecycleDeathPoll& out) {
    out = RecycleDeathPoll{};
    std::vector<DWORD> pids;
    out.snapshotOk = findWhServerPids(pids);
    if (!out.snapshotOk) return true;  // inconclusive, never absent
    out.whPids = pids;
    // A failed crash-report enumeration is inconclusive — NEVER "no
    // zombies": crashSnapshotOk=false keeps the poll out of the
    // fully-dead count (a truncated set is cleared by the overload, so
    // crashParentPids stays empty either way). The still-active arm below
    // is independent per-pid evidence and still runs.
    out.crashSnapshotOk = findCrashReportParentPids(out.crashParentPids);
    for (DWORD pid : censusPids) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h == nullptr) {
            // OpenProcess failure: never evidence of absence — but a plain
            // "process gone" error corroborates death while ACCESS_DENIED
            // (we are elevated, same-session) is a fail-closed recheck, so
            // it counts as still-active, not dead (§2 step 5c).
            if (GetLastError() == ERROR_ACCESS_DENIED) {
                out.stillActive.push_back(pid);
            }
            continue;
        }
        DWORD code = 0;
        if (GetExitCodeProcess(h, &code) && code == STILL_ACTIVE) {
            out.stillActive.push_back(pid);
        }
        CloseHandle(h);
    }
    return true;
}

bool WhRecycleSampler::zombiePoll(RecycleZombiePoll& out) {
    out = RecycleZombiePoll{};
    // Both enumerations are independent snapshotOk contracts: a failed
    // WHServer walk AND a failed crash-report walk are each inconclusive
    // (the bool overloads clear their sets on failure, so the sets stay
    // authoritative-or-empty either way).
    out.snapshotOk = findWhServerPids(out.whPids);
    out.crashSnapshotOk = findCrashReportParentPids(out.crashParentPids);
    return true;
}

double WhRecycleSampler::nowSecs() {
    return static_cast<double>(GetTickCount64()) / 1000.0;
}

void WhRecycleSampler::sleepMs(unsigned ms) {
    Sleep(ms);
}

// --- ComRecycleAttach (production step-4 seam) ---------------------------------
//
// Verbatim port of the proven tmp/wh_shutdown.cpp chain (KB §17), minus the
// printf instrumentation: CoGetClassObject(CLSID_Control) -> factory
// vtbl+0x20 create -> QI IDispatch -> GetIDsOfNames("WHouseEx") ->
// Invoke(DISPATCH_PROPERTYGET, args reversed [0]=I4 Create=1, [1]=BSTR
// MyWork) -> GetIDsOfNames("ForceShutDown") -> Invoke(DISPATCH_METHOD).
// Refs stay held until release() (§2 step 6). CoInitializeEx on the worker
// STA returns S_FALSE (the TsDevLib apartment guard already initialized it);
// CoUninitialize is NEVER called — the apartment belongs to the guard.

ComRecycleAttach::ComRecycleAttach(const std::wstring& myWorkDir)
    : m_myWorkDir(myWorkDir) {
    VariantInit(&m_vres);
}

namespace {

// HRESULTs consistent with the server having died between census-pass and
// attach (§2 step 4): treat as recycle-achieved -> skip to verify-death.
bool isServerDiedHr(HRESULT hr) {
    return hr == RPC_E_SERVER_DIED ||      // 0x8001011A
           hr == RPC_E_DISCONNECTED ||     // 0x80010108
           hr == CO_E_OBJNOTCONNECTED ||   // 0x800401FD
           hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE);  // 0x800706BA
}

std::string hrDetail(const char* stage, HRESULT hr) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s hr=0x%08lX", stage,
                  static_cast<unsigned long>(hr));
    return buf;
}

} // namespace

RecycleAttachStatus ComRecycleAttach::attachAndForceShutDown(
    const std::vector<DWORD>& censusPids, std::string& detail) {
    // Hard precondition (§2 step 4): NEVER invoke WHouseEx with Create=1
    // against an unresolved or guessed MyWork path — Create=1 authorizes
    // the class to instantiate against it.
    if (m_myWorkDir.empty()) {
        detail = "mywork_unresolved";
        return RecycleAttachStatus::AttachFailed;
    }

    // Image gate (whserver_census.h SSOT): the raw-COM chain below is a
    // layout fact of the ONE probed WHServer.exe build — hardcoded vtable
    // offsets and dispids. A TradeStation update that relayouts the binary
    // makes every offset wrong while still resolving, so EVERY census pid
    // must verify before any COM call. The production census sampler gates
    // the same check upstream; this repeats it here so the attach site is
    // safe even when reached without the census (cheap file reads, one
    // per pid, once per attach).
    for (DWORD pid : censusPids) {
        if (!whServerPidImageIsKnown(pid)) {
            detail = "whserver_image_unknown pid=" +
                std::to_string(static_cast<unsigned long>(pid));
            return RecycleAttachStatus::AttachFailed;
        }
    }

    // Bound the synchronous RPCs with the production dispatch watchdog —
    // a wedged server fires exit 15, fail-closed (§2 step 4).
    DispatchInFlightGuard guard("WhServerRecycle", 0);

    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(coHr) && coHr != S_FALSE) {
        detail = hrDetail("CoInitializeEx", coHr);
        return RecycleAttachStatus::AttachFailed;
    }

    // Multi-pid contract: run the sequence once per pid. Class-to-pid
    // binding is ambiguous under >1 pid but MOOT — verify-death requires
    // EVERY enumerated pid gone. Any non-death failure refuses (§2 step 4:
    // "any failed call -> release what we hold -> refuse"); a death-shaped
    // failure is recycle-achieved. Empty set is unreachable (the engine
    // short-circuits the census-absent case) but treated as ServerDied.
    bool anyOk = false;
    bool anyDied = censusPids.empty();
    for (DWORD pid : censusPids) {
        (void)pid;  // binding is ambiguous by design; see comment above
        release();  // drop refs from any prior per-pid iteration

        CLSID clsidCtl; IID iidFactory, iidCtl;
        CLSIDFromString(L"{7CEEA90C-D59E-11D3-8394-00C04F7BDF10}", &clsidCtl);
        CLSIDFromString(L"{7CEEACBC-D59E-11D3-8394-00C04F7BDF10}", &iidFactory);
        CLSIDFromString(L"{7CEEA90A-D59E-11D3-8394-00C04F7BDF10}", &iidCtl);

        HRESULT hr = CoGetClassObject(clsidCtl, CLSCTX_ALL, nullptr,
                                      iidFactory, (void**)&m_fact);
        if (FAILED(hr) || !m_fact) {
            detail = hrDetail("CoGetClassObject", hr);
            if (isServerDiedHr(hr)) { anyDied = true; continue; }
            return RecycleAttachStatus::AttachFailed;
        }

        typedef HRESULT (__stdcall *FactoryCreateFn)(
            IUnknown* self, unsigned a0, unsigned a1, const IID* riid,
            BSTR name, void** outObj, BSTR* outName);
        void** vt = *reinterpret_cast<void***>(m_fact);
        FactoryCreateFn create = (FactoryCreateFn)vt[8];  // vtbl+0x20

        const unsigned int tick = GetTickCount();
        const unsigned int scrambled =
            _lrotr(((tick >> 12) & 0xFFFF | 0x160000u) ^ 0x1256abefu, 6);
        wchar_t nameBuf[64];
        _snwprintf_s(nameBuf, 64, _TRUNCATE, L"%u,%u", 268435478u, scrambled);
        BSTR nm = SysAllocString(nameBuf);
        void* outObj = nullptr;
        hr = create(m_fact, 0, 0, &iidCtl, nm, &outObj, &m_outName);
        if (nm) SysFreeString(nm);
        if (FAILED(hr) || !outObj) {
            detail = hrDetail("factory_create", hr);
            if (isServerDiedHr(hr)) { anyDied = true; continue; }
            return RecycleAttachStatus::AttachFailed;
        }
        m_ctl = reinterpret_cast<IUnknown*>(outObj);

        hr = m_ctl->QueryInterface(IID_IDispatch, (void**)&m_dctrl);
        if (FAILED(hr) || !m_dctrl) {
            detail = hrDetail("QI_IDispatch", hr);
            if (isServerDiedHr(hr)) { anyDied = true; continue; }
            return RecycleAttachStatus::AttachFailed;
        }

        LPOLESTR wxn = const_cast<LPOLESTR>(L"WHouseEx");
        DISPID wxid = 0;
        hr = m_dctrl->GetIDsOfNames(IID_NULL, &wxn, 1, LOCALE_SYSTEM_DEFAULT,
                                    &wxid);
        if (FAILED(hr)) {
            detail = hrDetail("GetIDsOfNames(WHouseEx)", hr);
            if (isServerDiedHr(hr)) { anyDied = true; continue; }
            return RecycleAttachStatus::AttachFailed;
        }

        // rgvarg REVERSED: [0]=Create(I4=1), [1]=DirName(BSTR MyWork).
        VARIANT vargs[2];
        VariantInit(&vargs[0]); vargs[0].vt = VT_I4; vargs[0].lVal = 1;
        VariantInit(&vargs[1]); vargs[1].vt = VT_BSTR;
        vargs[1].bstrVal = SysAllocString(m_myWorkDir.c_str());
        DISPPARAMS dp = {}; dp.rgvarg = vargs; dp.cArgs = 2;
        VariantClear(&m_vres); VariantInit(&m_vres);
        EXCEPINFO exc{}; UINT argerr = 0;
        hr = m_dctrl->Invoke(wxid, IID_NULL, LOCALE_SYSTEM_DEFAULT,
                             DISPATCH_PROPERTYGET, &dp, &m_vres, &exc, &argerr);
        if (exc.bstrSource) SysFreeString(exc.bstrSource);
        if (exc.bstrDescription) SysFreeString(exc.bstrDescription);
        if (exc.bstrHelpFile) SysFreeString(exc.bstrHelpFile);
        VariantClear(&vargs[1]);
        if (FAILED(hr) || m_vres.vt != VT_DISPATCH || !m_vres.pdispVal) {
            detail = hrDetail("Invoke(WHouseEx)", FAILED(hr) ? hr : E_FAIL);
            if (isServerDiedHr(hr)) { anyDied = true; continue; }
            return RecycleAttachStatus::AttachFailed;
        }
        // Transfer ownership OUT of the VARIANT: m_whouse holds the single
        // owned ref and release() clears m_vres unconditionally — leaving
        // pdispVal set would make m_whouse->Release() + VariantClear a
        // double-release UAF on the alive-server refuse paths (review F1).
        m_whouse = m_vres.pdispVal;
        m_vres.vt = VT_EMPTY;
        m_vres.pdispVal = nullptr;

        LPOLESTR fsn = const_cast<LPOLESTR>(L"ForceShutDown");
        DISPID fsid = 0;
        hr = m_whouse->GetIDsOfNames(IID_NULL, &fsn, 1,
                                     LOCALE_SYSTEM_DEFAULT, &fsid);
        if (FAILED(hr)) {
            detail = hrDetail("GetIDsOfNames(ForceShutDown)", hr);
            if (isServerDiedHr(hr)) { anyDied = true; continue; }
            return RecycleAttachStatus::ForceShutDownFailed;
        }

        DISPPARAMS dp0 = {};
        VARIANT vr; VariantInit(&vr);
        EXCEPINFO exc2{}; UINT ae2 = 0;
        hr = m_whouse->Invoke(fsid, IID_NULL, LOCALE_SYSTEM_DEFAULT,
                              DISPATCH_METHOD, &dp0, &vr, &exc2, &ae2);
        if (exc2.bstrSource) SysFreeString(exc2.bstrSource);
        if (exc2.bstrDescription) SysFreeString(exc2.bstrDescription);
        if (exc2.bstrHelpFile) SysFreeString(exc2.bstrHelpFile);
        VariantClear(&vr);
        if (FAILED(hr)) {
            detail = hrDetail("Invoke(ForceShutDown)", hr);
            if (isServerDiedHr(hr)) { anyDied = true; continue; }
            return RecycleAttachStatus::ForceShutDownFailed;
        }
        anyOk = true;
    }

    detail.clear();
    if (anyOk) return RecycleAttachStatus::Ok;
    if (anyDied) return RecycleAttachStatus::ServerDied;
    return RecycleAttachStatus::Ok;  // unreachable: per-pid loop always
                                     // returns or sets anyOk
}

void ComRecycleAttach::release() {
    // RPC errors on an already-dead server are expected here
    // (wh_shutdown.cpp exercised this path); Release() return values are
    // deliberately ignored and never reported.
    if (m_whouse) { m_whouse->Release(); m_whouse = nullptr; }
    VariantClear(&m_vres);  // m_vres is always VariantInit'ed (ctor)
    if (m_dctrl)  { m_dctrl->Release();  m_dctrl = nullptr; }
    if (m_ctl)    { m_ctl->Release();    m_ctl = nullptr; }
    if (m_fact)   { m_fact->Release();   m_fact = nullptr; }
    if (m_outName) { SysFreeString(m_outName); m_outName = nullptr; }
    // CoUninitialize is NOT called — the apartment belongs to TsDevLib's
    // guard and is torn down by it (§2 step 6).
}

// --- Path helper ----------------------------------------------------------------

std::wstring resolveMyWorkDirFromBinPath(const char* myWorkBinPath) {
    if (myWorkBinPath == nullptr || myWorkBinPath[0] == '\0') return L"";
    std::string bin = myWorkBinPath;
    // Strip a trailing slash, then the final path component ("Bin").
    while (!bin.empty() &&
           (bin.back() == '\\' || bin.back() == '/')) {
        bin.pop_back();
    }
    const std::string::size_type pos = bin.find_last_of("\\/");
    if (pos == std::string::npos || pos == 0) return L"";
    const std::string dir = bin.substr(0, pos);
    const int wlen = MultiByteToWideChar(CP_ACP, 0, dir.c_str(),
                                         static_cast<int>(dir.size()),
                                         nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_ACP, 0, dir.c_str(), static_cast<int>(dir.size()),
                        &w[0], wlen);
    return w;
}

} // namespace tsdev
