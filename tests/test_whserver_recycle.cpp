// test_whserver_recycle.cpp
//
// Unit test for src/whserver_recycle.cpp: the WHServer gated auto-recycle
// engine (tmp/RECYCLE_ENGINE_DESIGN.md §2 steps 1-6 — R3 "engine skeleton:
// census->refuse path + all refuse exits; ForceShutDown behind the
// IRecycleAttach seam; process reads/clock behind IRecycleSampler").
//
// This is a PURE UNIT TEST. runWhserverRecycle is driven by injected fakes:
// a scripted census/death poll stream + a scripted attach result. No
// WHServer, no COM attach, no orlib20, no TradeStation contact. The
// production ComRecycleAttach TU is compiled in but never invoked
// (attachAndForceShutDown on the empty-MyWork path is exercised — it must
// refuse WITHOUT touching COM, the §2 step-4 hard precondition).
//
// The implementation .cpp files are compiled directly into this test
// executable (see CMakeLists.txt), so no library link is needed.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

#include "../src/whserver_recycle.h"
#include "../src/whserver_census.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "  FAIL: %s (exit %d)\n", msg, code);
    return code;
}

// --- scripted-stream builders -------------------------------------------------

tsdev::CensusPidRead lockRead(DWORD pid, DWORD lock) {
    tsdev::CensusPidRead r;
    r.pid = pid;
    r.attempted = true;
    r.imageKnown = true;
    r.sample = tsdev::WhLockSample{};
    r.sample.lockValue = lock;
    return r;
}

tsdev::CensusPidRead errRead(DWORD pid, DWORD openErr, DWORD rpmErr) {
    tsdev::CensusPidRead r;
    r.pid = pid;
    r.attempted = true;
    r.imageKnown = true;
    r.sample = tsdev::WhLockSample{};
    r.sample.openError = openErr;
    r.sample.rpmError = rpmErr;
    return r;
}

// A read on a pid whose image is not the probed WHServer build
// (imageKnown=false; the lock==0 bytes are a stale-VA artifact).
tsdev::CensusPidRead unknownImageRead(DWORD pid) {
    tsdev::CensusPidRead r;
    r.pid = pid;
    r.attempted = true;
    r.imageKnown = false;
    r.sample = tsdev::WhLockSample{};
    return r;
}

tsdev::CensusPoll zeroPoll(double t, DWORD pid) {
    tsdev::CensusPoll p;
    p.timeSecs = t;
    p.snapshotOk = true;
    p.pids = {pid};
    p.reads = {lockRead(pid, 0)};
    return p;
}

// A passing census for `pid`: lock==0 sustained across the 3.0 s window.
std::vector<tsdev::CensusPoll> passCensus(DWORD pid, double step = 0.2,
                                        double window = 3.2) {
    std::vector<tsdev::CensusPoll> v;
    for (double t = 0.0; t <= window; t += step) v.push_back(zeroPoll(t, pid));
    return v;
}

// A lock>0 census (foreign client) — decideCensus refuses at budget end.
std::vector<tsdev::CensusPoll> lockedCensus(DWORD pid, double step = 0.2,
                                          double span = 0.8) {
    std::vector<tsdev::CensusPoll> v;
    for (double t = 0.0; t <= span; t += step) {
        tsdev::CensusPoll p;
        p.timeSecs = t;
        p.snapshotOk = true;
        p.pids = {pid};
        p.reads = {lockRead(pid, 6)};
        v.push_back(p);
    }
    return v;
}

tsdev::RecycleDeathPoll deadPoll() {
    tsdev::RecycleDeathPoll d;
    d.snapshotOk = true;      // census pid absent from every set
    d.crashSnapshotOk = true; // crash-report re-enumeration succeeded
    return d;
}

std::vector<tsdev::RecycleDeathPoll> deadStream(int n) {
    return std::vector<tsdev::RecycleDeathPoll>(
        static_cast<size_t>(n), deadPoll());
}

// A clean zombie-gate poll: both re-enumerations conclusive, no WHServer
// parenting a crash reporter.
tsdev::RecycleZombiePoll cleanZombiePoll() {
    tsdev::RecycleZombiePoll z;
    z.snapshotOk = true;
    z.crashSnapshotOk = true;
    return z;
}

// A zombie-gate poll reporting a crash-frozen WHServer corpse: `pid`
// enumerates AND parents a TSCrashReport.exe.
tsdev::RecycleZombiePoll zombiePollFor(DWORD pid) {
    tsdev::RecycleZombiePoll z = cleanZombiePoll();
    z.whPids = {pid};
    z.crashParentPids = {pid};
    return z;
}

// --- fakes ---------------------------------------------------------------------

struct FakeSampler : tsdev::IRecycleSampler {
    std::vector<tsdev::CensusPoll>      census;
    size_t                              ci = 0;
    std::vector<tsdev::RecycleZombiePoll> zombies;
    size_t                              zi = 0;
    std::vector<tsdev::RecycleDeathPoll> deaths;
    size_t                              di = 0;
    double                              t = 0.0;
    std::vector<std::string>*           log;

    explicit FakeSampler(std::vector<std::string>* l) : log(l) {}

    bool censusPoll(tsdev::CensusPoll& out) override {
        log->push_back("census");
        if (ci >= census.size()) return false;
        out = census[ci++];
        return true;
    }
    bool zombiePoll(tsdev::RecycleZombiePoll& out) override {
        log->push_back("zombie");
        if (zi >= zombies.size()) return false;
        out = zombies[zi++];
        return true;
    }
    bool deathPoll(const std::vector<DWORD>&,
                   tsdev::RecycleDeathPoll& out) override {
        log->push_back("death");
        if (di >= deaths.size()) return false;
        out = deaths[di++];
        return true;
    }
    double nowSecs() override { return t; }
    void   sleepMs(unsigned ms) override { t += ms / 1000.0; }
};

struct FakeAttach : tsdev::IRecycleAttach {
    tsdev::RecycleAttachStatus status = tsdev::RecycleAttachStatus::Ok;
    int                        calls = 0;
    int                        releases = 0;
    std::vector<DWORD>         lastPids;
    std::string                detail = "fake-attach";
    std::vector<std::string>*  log;

    explicit FakeAttach(std::vector<std::string>* l) : log(l) {}

    tsdev::RecycleAttachStatus attachAndForceShutDown(
        const std::vector<DWORD>& pids, std::string& d) override {
        log->push_back("attach");
        ++calls;
        lastPids = pids;
        d = detail;
        return status;
    }
    void release() override {
        log->push_back("release");
        ++releases;
    }
};

// Convenience: a default RecycleParams with the fake seams installed.
struct Fixture {
    std::vector<std::string> log;
    FakeSampler              sampler{&log};
    FakeAttach               attach{&log};
    tsdev::RecycleParams     rp;
    int                      closeCalls = 0;
    bool                     closeResult = true;

    Fixture() {
        rp.sampler = &sampler;
        rp.attach  = &attach;
        rp.closeSession = [this] {
            log.push_back("close");
            ++closeCalls;
            return closeResult;
        };
        // Small bounds keep the scripted streams short.
        rp.censusWindowSecs = 3.0;
        rp.censusBudgetSecs = 30.0;
        rp.verifyBoundSecs  = 1.0;
        rp.pollIntervalSecs = 0.2;
        // A generous clean zombie stream: the pre-close/pre-attach corpse
        // gates consume one poll each on every path that reaches them.
        // Tests needing a zombie/inconclusive/exhausted gate overwrite
        // this stream (clear() scripts the exhausted -> inconclusive arm).
        sampler.zombies.assign(8, cleanZombiePoll());
    }

    tsdev::RecycleResult run() {
        return tsdev::runWhserverRecycle(rp);
    }
};

bool hasCall(const std::vector<std::string>& log, const char* s) {
    for (const auto& e : log) if (e == s) return true;
    return false;
}

// First index of `s` in log, or -1.
int idxOf(const std::vector<std::string>& log, const char* s) {
    for (size_t i = 0; i < log.size(); ++i)
        if (log[i] == s) return static_cast<int>(i);
    return -1;
}

} // namespace

int main() {
    std::printf("test_whserver_recycle\n");

    // -- Group 1: step-1 quiesce refuse -----------------------------------------
    {
        Fixture f;
        f.rp.dispatchInFlight = true;
        f.rp.ownSessionExists = true;
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseQuiesce ||
            r.exitCode != 19 || !r.terminateSelf || r.safeToDeleteLib ||
            r.reason != "quiesce_in_flight") {
            return fail("quiesce refuse w/ session -> terminateSelf(19)", 10);
        }
        if (f.closeCalls != 0 || !f.log.empty() && hasCall(f.log, "census") ||
            f.attach.calls != 0) {
            return fail("quiesce refuse must stop before close/census", 11);
        }
    }
    {
        Fixture f;
        f.rp.dispatchInFlight = true;
        f.rp.ownSessionExists = false;
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseQuiesce ||
            r.terminateSelf || !r.safeToDeleteLib) {
            return fail("quiesce refuse never-attached -> normal teardown", 12);
        }
        if (f.closeCalls != 0) {
            return fail("never-attached must never call closeSession", 13);
        }
    }

    // -- Group 2: step-2 close_failed -> terminateSelf contract ------------------
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.closeResult = false;
        f.sampler.census = passCensus(1111);
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseCloseFailed ||
            r.exitCode != 19 || !r.terminateSelf || r.safeToDeleteLib ||
            r.reason != "close_failed") {
            return fail("close_failed -> terminateSelf(19) contract", 20);
        }
        if (f.closeCalls != 1 || hasCall(f.log, "census") ||
            f.attach.calls != 0) {
            return fail("close_failed must stop before census/attach", 21);
        }
    }

    // -- Group 3: step-2 skipped on never-attached (F4) ---------------------------
    {
        Fixture f;
        f.rp.ownSessionExists = false;
        f.sampler.census = passCensus(1111);
        f.sampler.deaths = deadStream(3);
        f.attach.status = tsdev::RecycleAttachStatus::Ok;
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::Recycled || r.exitCode != 18) {
            return fail("never-attached happy path -> exit 18", 30);
        }
        if (f.closeCalls != 0) {
            return fail("step-2 must be SKIPPED when no session exists", 31);
        }
    }

    // -- Group 4: census refuse (lock>0 at budget) --------------------------------
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.rp.censusBudgetSecs = 0.5;   // locked census crosses the budget
        f.sampler.census = lockedCensus(2222);
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseCensus ||
            r.exitCode != 19 || r.reason != "census_lock" ||
            !r.safeToDeleteLib || r.terminateSelf) {
            return fail("census lock>0 at budget -> refuse 19", 40);
        }
        if (f.attach.calls != 0 || f.closeCalls != 1) {
            return fail("census refuse must run close but never attach", 41);
        }
    }

    // -- Group 5: census indeterminate (RPM error on listed pid) ------------------
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        tsdev::CensusPoll p;
        p.timeSecs = 0.0;
        p.snapshotOk = true;
        p.pids = {3333};
        p.reads = {errRead(3333, 0, /*rpmErr=*/299)};
        f.sampler.census = {p};
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseCensus ||
            r.reason != "census_indeterminate" || r.exitCode != 19) {
            return fail("RPM error on listed pid -> census_indeterminate", 50);
        }
        if (f.attach.calls != 0) {
            return fail("indeterminate census must never attach", 51);
        }
    }

    // -- Group 6: happy path + stage ordering --------------------------------------
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.sampler.census = passCensus(4444);
        f.sampler.deaths = deadStream(3);
        f.attach.status = tsdev::RecycleAttachStatus::Ok;
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::Recycled ||
            r.exitCode != 18 || !r.safeToDeleteLib || r.terminateSelf) {
            return fail("happy path -> Recycled exit 18", 60);
        }
        if (f.attach.calls != 1 || f.attach.releases != 1 ||
            f.attach.lastPids.size() != 1 ||
            f.attach.lastPids[0] != 4444) {
            return fail("attach called once with the census pid set", 61);
        }
        const int c = idxOf(f.log, "close");
        const int ce = idxOf(f.log, "census");
        const int a = idxOf(f.log, "attach");
        const int d = idxOf(f.log, "death");
        const int rel = idxOf(f.log, "release");
        if (c < 0 || ce < 0 || a < 0 || d < 0 || rel < 0 ||
            !(c < ce && ce < a && a < d && d < rel)) {
            return fail("stage order: close<census<attach<death<release", 62);
        }
    }

    // -- Group 7: attach_failed ---------------------------------------------------
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.sampler.census = passCensus(5555);
        f.sampler.deaths = deadStream(3);
        f.attach.status = tsdev::RecycleAttachStatus::AttachFailed;
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseAttach ||
            r.exitCode != 19 || r.reason != "attach_failed" ||
            !r.safeToDeleteLib) {
            return fail("attach_failed -> refuse 19", 70);
        }
        if (f.attach.releases != 1 || hasCall(f.log, "death")) {
            return fail("attach_failed releases refs, skips verify", 71);
        }
    }

    // -- Group 8: forceshutdown_failed --------------------------------------------
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.sampler.census = passCensus(5555);
        f.attach.status = tsdev::RecycleAttachStatus::ForceShutDownFailed;
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseForceShutDown ||
            r.reason != "forceshutdown_failed" || r.exitCode != 19) {
            return fail("forceshutdown_failed -> refuse 19", 80);
        }
        if (f.attach.releases != 1) {
            return fail("forceshutdown_failed releases refs", 81);
        }
    }

    // -- Group 9: ServerDied attach -> verify-death path ---------------------------
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.sampler.census = passCensus(6666);
        f.sampler.deaths = deadStream(3);
        f.attach.status = tsdev::RecycleAttachStatus::ServerDied;
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::Recycled || r.exitCode != 18) {
            return fail("server died at attach -> verify -> exit 18", 90);
        }
        if (!hasCall(f.log, "death")) {
            return fail("ServerDied must still run verify-death", 91);
        }
    }

    // -- Group 10: zombie persists -> zombie_present --------------------------------
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.rp.verifyBoundSecs = 0.5;
        f.sampler.census = passCensus(7777);
        tsdev::RecycleDeathPoll z;
        z.snapshotOk = true;
        z.crashSnapshotOk = true;
        z.whPids = {7777};
        z.crashParentPids = {7777};
        z.stillActive = {7777};
        f.sampler.deaths = {z, z, z, z, z};
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseVerifyDeath ||
            r.reason != "zombie_present" || r.exitCode != 19) {
            return fail("crash-report-frozen pid -> zombie_present", 100);
        }
        if (f.attach.releases != 1) {
            return fail("verify refuse releases refs", 101);
        }
    }

    // -- Group 11: ForceShutDown inert (pid survives, no crash child) ---------------
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.rp.verifyBoundSecs = 0.5;
        f.sampler.census = passCensus(8888);
        tsdev::RecycleDeathPoll d;
        d.snapshotOk = true;
        d.crashSnapshotOk = true;
        d.whPids = {8888};
        d.stillActive = {8888};
        f.sampler.deaths = {d, d, d, d, d};
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseVerifyDeath ||
            r.reason != "forceshutdown_inert" || r.exitCode != 19) {
            return fail("surviving pid -> forceshutdown_inert", 110);
        }
    }

    // -- Group 12: census-absent arm — server already gone ---------------------------
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        // >=3 consecutive successful empty re-enumerations -> absent arm.
        for (int i = 0; i < 4; ++i) {
            tsdev::CensusPoll p;
            p.timeSecs = i * 0.2;
            p.snapshotOk = true;
            f.sampler.census.push_back(p);
        }
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::Recycled || r.exitCode != 18) {
            return fail("re-enumerated absence -> recycled-equivalent", 120);
        }
        if (f.attach.calls != 0 || hasCall(f.log, "death")) {
            return fail("absent-at-census skips attach AND verify", 121);
        }
    }

    // -- Group 13: multi-pid census + all-pids verify ---------------------------------
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.sampler.census.clear();
        for (double t = 0.0; t <= 3.2; t += 0.2) {
            tsdev::CensusPoll p;
            p.timeSecs = t;
            p.snapshotOk = true;
            p.pids = {1001, 1002};
            p.reads = {lockRead(1001, 0), lockRead(1002, 0)};
            f.sampler.census.push_back(p);
        }
        // Death: pid 1002 still active for the first two polls, then gone —
        // needs >=3 consecutive fully-dead polls.
        tsdev::RecycleDeathPoll alive;
        alive.snapshotOk = true;
        alive.crashSnapshotOk = true;
        alive.whPids = {1002};
        alive.stillActive = {1002};
        f.sampler.deaths = {alive, alive,
                            deadPoll(), deadPoll(), deadPoll()};
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::Recycled) {
            return fail("multi-pid verify requires EVERY pid absent", 130);
        }
        if (f.attach.lastPids.size() != 2 ||
            f.attach.lastPids[0] != 1001 || f.attach.lastPids[1] != 1002) {
            return fail("attach receives the union of census pids", 131);
        }
    }

    // -- Group 14: verify bound honored ----------------------------------------------
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        // Bound 0.3 s with 200 ms polls -> at most 2 death polls, never the
        // 3 consecutive confirmations required.
        f.rp.verifyBoundSecs = 0.3;
        f.sampler.census = passCensus(9999);
        f.sampler.deaths = deadStream(10);
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseVerifyDeath) {
            return fail("verify bound refuses when confirm never reaches", 140);
        }
    }

    // -- Group 14b: crash-report enumeration inconclusive => never dead ------
    // A failed TSCrashReport walk must not count toward fully-dead: the
    // frozen-reporter zombie check is blind, so deadStreak stays 0 and the
    // engine refuses at the verify bound (review F3).
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.rp.verifyBoundSecs = 0.5;
        f.sampler.census = passCensus(1357);
        tsdev::RecycleDeathPoll d;
        d.snapshotOk = true;          // WHServer re-enum succeeded
        d.crashSnapshotOk = false;    // crash-report walk inconclusive
        f.sampler.deaths = {d, d, d, d, d};
        tsdev::RecycleResult r = f.run();
        // P4 f-iv: every poll inconclusive => the honest refuse label is
        // death_unverifiable — nothing was ever observed alive OR dead.
        if (r.outcome != tsdev::RecycleOutcome::RefuseVerifyDeath ||
            r.reason != "death_unverifiable" || r.exitCode != 19) {
            return fail("inconclusive crash enum -> death_unverifiable", 141);
        }
        if (f.attach.releases != 1) {
            return fail("verify refuse releases refs", 142);
        }
    }

    // -- Group 15: MyWork path derivation + empty-path COM precondition ---------------
    {
        const std::wstring w = tsdev::resolveMyWorkDirFromBinPath(
            "C:\\TS\\MyWork\\Bin");
        if (w != L"C:\\TS\\MyWork") {
            return fail("resolveMyWorkDirFromBinPath strips \\Bin", 150);
        }
        if (!tsdev::resolveMyWorkDirFromBinPath("").empty() ||
            !tsdev::resolveMyWorkDirFromBinPath(nullptr).empty() ||
            !tsdev::resolveMyWorkDirFromBinPath("Bin").empty()) {
            return fail("unresolved MyWork -> empty", 151);
        }
        // Empty MyWork => AttachFailed WITHOUT touching COM (§2 step-4 hard
        // precondition) — safe to call in a unit test with no COM init.
        tsdev::ComRecycleAttach real(L"");
        std::string det;
        if (real.attachAndForceShutDown({1}, det) !=
                tsdev::RecycleAttachStatus::AttachFailed ||
            det != "mywork_unresolved") {
            return fail("empty MyWork refuses before any COM call", 152);
        }
    }

    // -- Group 15b: died-mid-census (review F9) — the census passes via the
    //    absent arm over a NON-EMPTY pid union: attach must NOT run (it
    //    would CoGetClassObject-spawn a FRESH WHServer the gate never
    //    measured) and verify-death is redundant (>=3 consecutive
    //    re-enumerated absences is the same evidence step 5 requires).
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        // pid listed once with lock==0, then >=3 consecutive successful
        // empty re-enumerations -> absent-arm Pass over union {4242}.
        f.sampler.census.push_back(zeroPoll(0.0, 4242));
        for (int i = 1; i <= 3; ++i) {
            tsdev::CensusPoll p;
            p.timeSecs = i * 0.2;
            p.snapshotOk = true;
            f.sampler.census.push_back(p);
        }
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::Recycled ||
            r.exitCode != 18 || !r.safeToDeleteLib) {
            return fail("died-mid-census absent arm -> recycled", 155);
        }
        if (f.attach.calls != 0 || hasCall(f.log, "death")) {
            return fail("absent-arm pass must skip attach AND verify", 156);
        }
    }

    // -- Group 15c: MIXED pass — one pid still listed+zero while another
    //    went absent-confirmed: attach MUST still run for the live pid,
    //    and only the live pid may be handed to the attach seam (P4 f-i:
    //    the confirmed-absent 4344 would fail the image gate — OpenProcess
    //    on a dead pid — blocking a legitimate recycle of 4343).
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        // 4343 on a sustained-zero run; 4344 vanishes after poll 0.
        for (double t = 0.0; t <= 3.2; t += 0.2) {
            tsdev::CensusPoll p;
            p.timeSecs = t;
            p.snapshotOk = true;
            if (t < 0.3) {
                p.pids = {4343, 4344};
                p.reads = {lockRead(4343, 0), lockRead(4344, 0)};
            } else {
                p.pids = {4343};
                p.reads = {lockRead(4343, 0)};
            }
            f.sampler.census.push_back(p);
        }
        f.sampler.deaths = deadStream(5);
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::Recycled) {
            return fail("mixed listed+absent census -> recycle", 157);
        }
        if (f.attach.calls != 1) {
            return fail("live listed pid must still be attached", 158);
        }
        if (f.attach.lastPids.size() != 1 ||
            f.attach.lastPids[0] != 4343) {
            return fail("attach must get ONLY the still-listed pid", 159);
        }
    }

    // -- Group 16: production WhRecycleSampler::deathPoll fail-closed
    //    branches on REAL processes (review F8; the R1 self-targeting
    //    convention — no TS, no WHServer needed). Exercises
    //    whserver_recycle.cpp's OpenProcess/STILL_ACTIVE arm, which the
    //    scripted FakeSampler never reaches.
    {
        tsdev::WhRecycleSampler s;

        // (a) Own pid: OpenProcess ok + STILL_ACTIVE -> stillActive
        //     (the alive/denied arm — the fail-closed direction).
        tsdev::RecycleDeathPoll dp;
        const DWORD self = GetCurrentProcessId();
        if (!s.deathPoll({self}, dp) || !dp.snapshotOk) {
            return fail("deathPoll on own pid must produce a poll", 160);
        }
        if (std::find(dp.stillActive.begin(), dp.stillActive.end(), self) ==
            dp.stillActive.end()) {
            return fail("own live pid must read stillActive", 161);
        }

        // (b) Spawned-then-exited pid: OpenProcess fails
        //     (ERROR_INVALID_PARAMETER) or GetExitCodeProcess != STILL_ACTIVE
        //     — either way it must NOT land in stillActive.
        STARTUPINFOA si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        char cmd[] = "cmd.exe /c exit 0";
        if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            return fail("CreateProcess for exited-pid probe failed", 162);
        }
        WaitForSingleObject(pi.hProcess, 10000);
        const DWORD deadPid = pi.dwProcessId;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        tsdev::RecycleDeathPoll dp2;
        if (!s.deathPoll({deadPid}, dp2) || !dp2.snapshotOk) {
            return fail("deathPoll on exited pid must produce a poll", 163);
        }
        if (std::find(dp2.stillActive.begin(), dp2.stillActive.end(),
                      deadPid) != dp2.stillActive.end()) {
            return fail("exited pid must NOT read stillActive", 164);
        }
    }

    // -- Group 17: image-identity gate at census level (R5) --------------------
    // A census pid whose image is not the probed WHServer build reads a
    // persistent 0 at the stale VA — the exact false-pass the gate exists
    // to prevent. decideCensus makes it Indeterminate => the engine
    // refuses census_indeterminate and never reaches attach.
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        tsdev::CensusPoll p;
        p.timeSecs = 0.0;
        p.snapshotOk = true;
        p.pids = {1212};
        p.reads = {unknownImageRead(1212)};
        f.sampler.census = {p};
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseCensus ||
            r.reason != "census_indeterminate" || r.exitCode != 19 ||
            !r.safeToDeleteLib) {
            return fail("unknown-image census pid -> census_indeterminate", 170);
        }
        if (f.attach.calls != 0) {
            return fail("unknown-image census must never attach", 171);
        }
    }

    // -- Group 18: attach-site identity gate (R5) ------------------------------
    // ComRecycleAttach must refuse BEFORE any COM call when a census pid's
    // image is not the probed build — the raw-COM chain's hardcoded vtable
    // offsets/dispids are layout facts of that one build, so the gate holds
    // even when a caller reaches attach directly (defense-in-depth behind
    // the census-level gate). Uses a RESOLVED MyWork so the check order is
    // unambiguous; own pid and an invalid pid both fail identity.
    {
        tsdev::ComRecycleAttach real(L"C:\\TS\\MyWork");
        std::string det;
        if (real.attachAndForceShutDown({GetCurrentProcessId()}, det) !=
                tsdev::RecycleAttachStatus::AttachFailed ||
            det.find("whserver_image_unknown") == std::string::npos) {
            return fail("attach must refuse an unprobed image pid", 180);
        }
        det.clear();
        if (real.attachAndForceShutDown({0xFFFFFFFF}, det) !=
                tsdev::RecycleAttachStatus::AttachFailed) {
            return fail("attach must refuse an unresolvable image pid", 181);
        }
        // The MyWork hard precondition still dominates when both fail:
        // empty path => mywork_unresolved before the identity check.
        tsdev::ComRecycleAttach noWork(L"");
        det.clear();
        if (noWork.attachAndForceShutDown({0xFFFFFFFF}, det) !=
                tsdev::RecycleAttachStatus::AttachFailed ||
            det != "mywork_unresolved") {
            return fail("empty MyWork still refuses before identity", 182);
        }
    }

    // -- Group 19: step-2 bounded close — a hanging close fires the bound ------
    // The close must stay on the engine thread (CELWHouse is bound per
    // (thread,path)), so the bound is the monitor thread. A close that
    // outlives closeBoundSecs fires onCloseBoundExceeded — production's
    // default is stderr + TerminateProcess(19); the injected stub records
    // the fire and unblocks the scripted close — and the engine then
    // refuses close_timeout on the same teardown contract: the late
    // return is never trusted and census/ForceShutDown are never reached.
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.rp.closeBoundSecs = 0.2;   // real 200 ms monitor bound
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ev == nullptr) {
            return fail("CreateEvent for bounded-close test", 183);
        }
        bool abortFired = false;     // written on the monitor thread,
                                     // read after the engine's join
        f.rp.closeSession = [&] {
            f.log.push_back("close");
            ++f.closeCalls;
            WaitForSingleObject(ev, INFINITE);  // hangs until the bound fires
            return true;
        };
        f.rp.onCloseBoundExceeded = [&] {
            abortFired = true;
            SetEvent(ev);            // release the scripted close
        };
        f.sampler.census = passCensus(2468);
        tsdev::RecycleResult r = f.run();
        CloseHandle(ev);
        if (!abortFired) {
            return fail("hanging close must fire the bound action", 184);
        }
        if (r.outcome != tsdev::RecycleOutcome::RefuseCloseFailed ||
            r.reason != "close_timeout" || r.exitCode != 19 ||
            !r.terminateSelf || r.safeToDeleteLib) {
            return fail("fired bound -> close_timeout terminateSelf(19)", 185);
        }
        if (hasCall(f.log, "census") || f.attach.calls != 0) {
            return fail("close_timeout must stop before census/attach", 186);
        }
    }

    // -- Group 20: step-2 bounded close — a fast close never fires ------------
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.rp.closeBoundSecs = 5.0;   // generous; the scripted close is instant
        bool abortFired = false;
        f.rp.onCloseBoundExceeded = [&] { abortFired = true; };
        f.sampler.census = passCensus(3579);
        f.sampler.deaths = deadStream(3);
        tsdev::RecycleResult r = f.run();
        if (abortFired) {
            return fail("fast close must never fire the bound action", 190);
        }
        if (r.outcome != tsdev::RecycleOutcome::Recycled ||
            r.exitCode != 18 || f.closeCalls != 1) {
            return fail("fast close under the bound -> normal path", 191);
        }
    }

    // -- Group 21: P4-(b) pre-close corpse gate --------------------------------
    // A crash-frozen WHServer still enumerates (a frozen corpse is not
    // absent), so the guard can trip on it and closeWarehouse would wedge
    // inside the dispatch — previously discovered only when the close
    // bound killed the process. The gate refuses BEFORE the close with
    // the stage-tagged reason (terminateSelf — an own session may still
    // be open; the close never ran).
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.sampler.zombies = {zombiePollFor(6060)};
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseCloseFailed ||
            r.reason != "zombie_present_close" || r.exitCode != 19 ||
            !r.terminateSelf || r.safeToDeleteLib ||
            r.detail != "pid=6060") {
            return fail("zombie before close -> zombie_present_close", 200);
        }
        if (f.closeCalls != 0 || hasCall(f.log, "census") ||
            f.attach.calls != 0) {
            return fail("pre-close zombie refuse stops before close/census",
                        201);
        }
        if (idxOf(f.log, "zombie") != 0) {
            return fail("zombie gate must run ahead of the close", 202);
        }
    }
    // Crash-report enumeration inconclusive => fail closed, its own label
    // (a failed walk is never "no zombies").
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        tsdev::RecycleZombiePoll z;
        z.snapshotOk = true;
        z.crashSnapshotOk = false;   // crash-report walk inconclusive
        f.sampler.zombies = {z};
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseCloseFailed ||
            r.reason != "zombie_unverifiable_close" || r.exitCode != 19 ||
            !r.terminateSelf || f.closeCalls != 0) {
            return fail("inconclusive -> zombie_unverifiable_close", 203);
        }
    }
    // An exhausted zombie stream is inconclusive the same way (never
    // proceeds on unknown zombie state).
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.sampler.zombies.clear();
        tsdev::RecycleResult r = f.run();
        if (r.reason != "zombie_unverifiable_close" || r.exitCode != 19 ||
            !r.terminateSelf || f.closeCalls != 0) {
            return fail("exhausted zombie stream -> unverifiable refuse", 204);
        }
    }
    // Never-attached site: the pre-close gate is SKIPPED (there is no
    // close to wedge); the corpse — whose frozen memory reads lock==0 —
    // passes the census and is caught by the PRE-ATTACH gate instead
    // (the exact never-attached hazard from P4-(b)).
    {
        Fixture f;
        f.rp.ownSessionExists = false;
        f.sampler.census = passCensus(6161);   // corpse reads lock==0
        f.sampler.zombies = {zombiePollFor(6161)};
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseAttach ||
            r.reason != "zombie_present_attach" || r.exitCode != 19 ||
            !r.safeToDeleteLib || r.terminateSelf ||
            r.detail != "pid=6161") {
            return fail("corpse passes census -> zombie_present_attach", 205);
        }
        if (f.attach.calls != 0 || f.closeCalls != 0 ||
            hasCall(f.log, "death")) {
            return fail("pre-attach zombie refuse: no attach/close/death",
                        206);
        }
        // Ordering proof the close gate never ran: the single zombie poll
        // fired AFTER the census, not before a close.
        const int z = idxOf(f.log, "zombie");
        const int ce = idxOf(f.log, "census");
        if (z < 0 || ce < 0 || z < ce || hasCall(f.log, "close")) {
            return fail("no-session: zombie gate first runs at attach", 207);
        }
    }
    // Inconclusive zombie check at the attach gate => fail closed.
    {
        Fixture f;
        f.rp.ownSessionExists = false;
        f.sampler.census = passCensus(6262);
        tsdev::RecycleZombiePoll z;
        z.snapshotOk = false;        // WHServer re-enum failed — inconclusive
        z.crashSnapshotOk = true;
        f.sampler.zombies = {z};
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseAttach ||
            r.reason != "zombie_unverifiable_attach" || r.exitCode != 19 ||
            !r.safeToDeleteLib || f.attach.calls != 0) {
            return fail("inconclusive -> zombie_unverifiable_attach", 208);
        }
    }

    // -- Group 22: closeBoundSecs<=0 params guard (P4 f-ii) --------------------
    // 0 fires the monitor instantly; a negative wraps the DWORD ms
    // conversion to ~49.7 days of silently-unbounded close. Both refuse
    // at engine entry — the close and every later stage never run.
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.rp.closeBoundSecs = 0.0;
        f.sampler.census = passCensus(7070);
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseCloseFailed ||
            r.reason != "close_bound_invalid" || r.exitCode != 19 ||
            !r.terminateSelf || r.safeToDeleteLib || f.closeCalls != 0 ||
            !f.log.empty()) {
            return fail("closeBoundSecs=0 -> close_bound_invalid refuse", 210);
        }
    }
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.rp.closeBoundSecs = -30.0;
        tsdev::RecycleResult r = f.run();
        if (r.reason != "close_bound_invalid" || r.exitCode != 19 ||
            !r.terminateSelf || f.closeCalls != 0) {
            return fail("negative closeBoundSecs wraps -> refuse", 211);
        }
    }
    {
        Fixture f;                      // never-attached: same params refuse
        f.rp.ownSessionExists = false;  // but normal teardown (no session)
        f.rp.closeBoundSecs = 0.0;
        f.sampler.census = passCensus(7071);
        tsdev::RecycleResult r = f.run();
        if (r.reason != "close_bound_invalid" || r.exitCode != 19 ||
            r.terminateSelf || !r.safeToDeleteLib || f.closeCalls != 0) {
            return fail("closeBoundSecs=0 never-attached -> refuse", 212);
        }
    }

    // -- Group 23: verify-death reason precision (P4 f-iv) ---------------------
    // Conclusive-dead polls that never reach the confirm streak plus an
    // inconclusive tail => death_unverifiable — the pid was never
    // observed alive, so "forceshutdown_inert" would be a guess.
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.rp.verifyBoundSecs = 0.5;
        f.sampler.census = passCensus(8181);
        tsdev::RecycleDeathPoll bad;
        bad.snapshotOk = false;      // WHServer re-enum inconclusive
        bad.crashSnapshotOk = true;
        f.sampler.deaths = {deadPoll(), deadPoll(), bad, bad, bad, bad};
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseVerifyDeath ||
            r.reason != "death_unverifiable" || r.exitCode != 19) {
            return fail("unconfirmed death -> death_unverifiable", 220);
        }
    }
    // A conclusive alive/inert observation followed by an inconclusive
    // tail keeps the inert label — it is positive evidence, not a guess.
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.rp.verifyBoundSecs = 0.5;
        f.sampler.census = passCensus(8282);
        tsdev::RecycleDeathPoll inert;
        inert.snapshotOk = true;
        inert.crashSnapshotOk = true;
        inert.whPids = {8282};
        inert.stillActive = {8282};
        tsdev::RecycleDeathPoll bad;
        bad.snapshotOk = true;
        bad.crashSnapshotOk = false;
        f.sampler.deaths = {inert, bad, bad, bad, bad, bad};
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseVerifyDeath ||
            r.reason != "forceshutdown_inert" || r.exitCode != 19) {
            return fail("observed-inert survives inconclusive tail", 221);
        }
    }

    // -- Group 24: census refuse reason precision (P4 f-iv) --------------------
    // A Refuse dominated by re-enumeration failures (a pid was listed
    // once, then every snapshot fails so the absent arm can never
    // confirm) is "census_unverifiable" — no held lock was ever observed;
    // the old "census_lock" labeled unverifiable evidence as a lock.
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.rp.censusBudgetSecs = 0.5;
        f.sampler.census.push_back(zeroPoll(0.0, 9191));   // listed once
        for (int i = 1; i <= 5; ++i) {  // then snapshots keep failing
            tsdev::CensusPoll p;
            p.timeSecs = i * 0.25;
            p.snapshotOk = false;
            f.sampler.census.push_back(p);
        }
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseCensus ||
            r.reason != "census_unverifiable" || r.exitCode != 19 ||
            !r.safeToDeleteLib) {
            return fail("snapshot-failure refuse -> census_unverifiable", 230);
        }
        if (f.attach.calls != 0 || f.closeCalls != 1) {
            return fail("census refuse still ran close, never attach", 231);
        }
    }
    // A sub-window zero stream ending without a verdict is the same
    // unverifiable class: quiescence was measured but never sustained —
    // and no lock>0 was ever observed.
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.sampler.census.clear();
        for (double t = 0.0; t <= 2.0; t += 0.2) {  // < 3.0 window, then ends
            f.sampler.census.push_back(zeroPoll(t, 9292));
        }
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseCensus ||
            r.reason != "census_unverifiable") {
            return fail("sub-window stream end -> census_unverifiable", 232);
        }
    }
    // A positively-observed held lock still labels census_lock even when
    // the tail went inconclusive — affirmative evidence wins the label.
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.rp.censusBudgetSecs = 0.5;
        f.sampler.census.clear();
        tsdev::CensusPoll locked;
        locked.timeSecs = 0.0;
        locked.snapshotOk = true;
        locked.pids = {9393};
        locked.reads = {lockRead(9393, 6)};    // a held lock WAS observed
        f.sampler.census.push_back(locked);
        for (int i = 1; i <= 5; ++i) {
            tsdev::CensusPoll p;
            p.timeSecs = i * 0.25;
            p.snapshotOk = false;
            f.sampler.census.push_back(p);
        }
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseCensus ||
            r.reason != "census_lock") {
            return fail("observed lock>0 + inconclusive tail -> census_lock",
                        233);
        }
    }

    // -- Group 25: pollIntervalSecs<=0 params guard (P4 nit — identical ----
    // wrap class to closeBoundSecs) ---------------------------------------
    // A negative interval wraps the static_cast<unsigned>(secs*1000.0)
    // sleepMs conversion to ~49.7 days at every census/verify-death poll
    // (a silently stalled engine); 0 is not a usable interval either. Both
    // refuse at engine entry — before the close and every later stage — on
    // the same params-invalid/teardown contract as close_bound_invalid.
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.rp.pollIntervalSecs = 0.0;
        f.sampler.census = passCensus(7171);
        tsdev::RecycleResult r = f.run();
        if (r.outcome != tsdev::RecycleOutcome::RefuseCloseFailed ||
            r.reason != "poll_interval_invalid" || r.exitCode != 19 ||
            !r.terminateSelf || r.safeToDeleteLib || f.closeCalls != 0 ||
            !f.log.empty()) {
            return fail("pollIntervalSecs=0 -> poll_interval_invalid refuse",
                        240);
        }
    }
    {
        Fixture f;
        f.rp.ownSessionExists = true;
        f.rp.pollIntervalSecs = -0.2;
        tsdev::RecycleResult r = f.run();
        if (r.reason != "poll_interval_invalid" || r.exitCode != 19 ||
            !r.terminateSelf || f.closeCalls != 0) {
            return fail("negative pollIntervalSecs wraps -> refuse", 241);
        }
    }
    {
        Fixture f;                      // never-attached: same params refuse
        f.rp.ownSessionExists = false;  // but normal teardown (no session)
        f.rp.pollIntervalSecs = 0.0;
        f.sampler.census = passCensus(7272);
        tsdev::RecycleResult r = f.run();
        if (r.reason != "poll_interval_invalid" || r.exitCode != 19 ||
            r.terminateSelf || !r.safeToDeleteLib || f.closeCalls != 0) {
            return fail("pollIntervalSecs=0 never-attached -> refuse", 242);
        }
    }

    // -- Group 26: boundedCloseWarehouse direct (R2 exported §5 close ----
    //    stage seam — the SAME machinery the engine's step-2 runs:
    //    zombie pre-check then the monitor-bounded close on the calling
    //    thread). The R2 attached-site defer drives it to release the
    //    +7 via graceful teardown; pin every outcome arm here so the
    //    engine refactor and the defer path share one pinned contract.
    {
        std::vector<std::string> log;
        std::string reason, detail;

        // (a) fast close inside the bound -> true; the zombie gate ran
        //     first (exactly one poll, ahead of the close call).
        {
            std::vector<std::string> ordLog;
            FakeSampler s(&ordLog);
            s.zombies.assign(2, cleanZombiePoll());
            int closeCalls = 0;
            if (!tsdev::boundedCloseWarehouse(
                    [&] { ordLog.push_back("close"); ++closeCalls;
                          return true; },
                    &s, /*boundSecs=*/5.0, /*onBoundExceeded=*/nullptr,
                    &reason, &detail) ||
                closeCalls != 1) {
                return fail("boundedClose: fast close must succeed", 250);
            }
            if (idxOf(ordLog, "zombie") != 0 ||
                idxOf(ordLog, "close") != 1) {
                return fail("boundedClose: gate must run before close",
                            251);
            }
        }

        // (b) close returned false -> close_failed (+ honest detail).
        {
            FakeSampler s(&log);
            s.zombies.assign(1, cleanZombiePoll());
            if (tsdev::boundedCloseWarehouse(
                    [] { return false; }, &s, 5.0, nullptr,
                    &reason, &detail) ||
                reason != "close_failed" ||
                detail.find("closeWarehouse failed") ==
                    std::string::npos) {
                return fail("boundedClose: false -> close_failed", 252);
            }
        }

        // (c) zombie gate ahead of the close -> zombie_present_close;
        //     the close never runs.
        {
            FakeSampler s(&log);
            s.zombies = {zombiePollFor(5151)};
            int closeCalls = 0;
            if (tsdev::boundedCloseWarehouse(
                    [&] { ++closeCalls; return true; }, &s, 5.0,
                    nullptr, &reason, &detail) ||
                reason != "zombie_present_close" ||
                detail != "pid=5151" || closeCalls != 0) {
                return fail("boundedClose: zombie -> "
                            "zombie_present_close", 253);
            }
        }

        // (d) inconclusive gate -> zombie_unverifiable_close (an
        //     exhausted stream AND a missing sampler are each never
        //     "no zombies").
        {
            FakeSampler s(&log);
            s.zombies.clear();
            if (tsdev::boundedCloseWarehouse(
                    [] { return true; }, &s, 5.0, nullptr,
                    &reason, &detail) ||
                reason != "zombie_unverifiable_close") {
                return fail("boundedClose: exhausted gate -> "
                            "unverifiable", 254);
            }
            if (tsdev::boundedCloseWarehouse(
                    [] { return true; }, nullptr, 5.0, nullptr,
                    &reason, &detail) ||
                reason != "zombie_unverifiable_close") {
                return fail("boundedClose: null sampler -> "
                            "unverifiable", 255);
            }
        }

        // (e) boundSecs<=0 -> close_bound_invalid; the gate and the
        //     close never run (a fresh log proves no poll was consumed).
        {
            std::vector<std::string> eLog;
            FakeSampler s(&eLog);
            int closeCalls = 0;
            if (tsdev::boundedCloseWarehouse(
                    [&] { ++closeCalls; return true; }, &s, 0.0,
                    nullptr, &reason, &detail) ||
                reason != "close_bound_invalid" || closeCalls != 0 ||
                !eLog.empty()) {
                return fail("boundedClose: bound<=0 -> "
                            "close_bound_invalid", 256);
            }
        }

        // (f) empty close seam -> close_failed (the seam detail).
        {
            FakeSampler s(&log);
            s.zombies.assign(1, cleanZombiePoll());
            if (tsdev::boundedCloseWarehouse(
                    {}, &s, 5.0, nullptr, &reason, &detail) ||
                reason != "close_failed" ||
                detail.find("closeSession seam empty") ==
                    std::string::npos) {
                return fail("boundedClose: empty seam -> "
                            "close_failed", 257);
            }
        }

        // (g) hanging close + non-fatal injected action -> the bound
        //     fires, the action runs on the monitor thread, and the
        //     result is close_timeout — the late return is untrusted.
        {
            FakeSampler s(&log);
            s.zombies.assign(1, cleanZombiePoll());
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (ev == nullptr) {
                return fail("boundedClose: CreateEvent", 258);
            }
            bool fired = false;
            const bool ok = tsdev::boundedCloseWarehouse(
                [&] { WaitForSingleObject(ev, INFINITE); return true; },
                &s, /*boundSecs=*/0.2,
                [&] { fired = true; SetEvent(ev); },
                &reason, &detail);
            CloseHandle(ev);
            if (ok || !fired || reason != "close_timeout") {
                return fail("boundedClose: bound fire -> "
                            "close_timeout", 259);
            }
        }

        // (h) a fast close never fires the bound action.
        {
            FakeSampler s(&log);
            s.zombies.assign(1, cleanZombiePoll());
            bool fired = false;
            if (!tsdev::boundedCloseWarehouse(
                    [] { return true; }, &s, 5.0,
                    [&] { fired = true; }, &reason, &detail) ||
                fired) {
                return fail("boundedClose: fast close fired the bound",
                            260);
            }
        }
    }

    std::printf("  all checks passed\n");
    return 0;
}
