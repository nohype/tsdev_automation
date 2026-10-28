// test_whserver_census.cpp
//
// Unit test for src/whserver_census.cpp: the WHServer m_nLock census gate
// (tmp/RECYCLE_ENGINE_DESIGN.md §3.3/§3.4 — R1 "census core").
//
// This is a PURE UNIT TEST. decideCensus is driven entirely by scripted
// CensusPoll streams — WHServer is never required to run, no orlib20, no
// TsDevLib, no TradeStation contact. The level-2 seam readWhLockSample is
// exercised against THIS test process: OpenProcess self + ReadProcessMemory
// of known statics at their own VAs (same convention as
// test_whserver_health.cpp measuring its own VAS).
//
// The implementation .cpp files are compiled directly into this test
// executable (see CMakeLists.txt), so no library link is needed.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

#include "../src/whserver_census.h"
#include "../src/whserver_health.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Statics read back through REAL ReadProcessMemory in group 9.
DWORD  g_lockSim = 7;
uint8_t g_actSim = 1;

int fail(const char* msg, int code) {
    std::fprintf(stderr, "  FAIL: %s (exit %d)\n", msg, code);
    return code;
}

// --- scripted-stream builders ------------------------------------------------

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

tsdev::CensusPidRead noRead(DWORD pid) {
    tsdev::CensusPidRead r;
    r.pid = pid;
    r.attempted = false;
    r.imageKnown = false;
    r.sample = tsdev::WhLockSample{};
    return r;
}

// A read attempted against a pid whose image is NOT the probed WHServer
// build: production skips the RPM, leaving a zeroed sample — the lock==0
// bytes must never count as evidence (the stale-VA false-pass).
tsdev::CensusPidRead unknownImageRead(DWORD pid) {
    tsdev::CensusPidRead r;
    r.pid = pid;
    r.attempted = true;
    r.imageKnown = false;
    r.sample = tsdev::WhLockSample{};
    return r;
}

// A snapshotOk poll listing `pids`, with reads for exactly those pids.
tsdev::CensusPoll okPoll(double t, std::vector<DWORD> pids,
                       std::vector<tsdev::CensusPidRead> reads) {
    tsdev::CensusPoll p;
    p.timeSecs = t;
    p.snapshotOk = true;
    p.pids = pids;
    p.reads = reads;
    return p;
}

// A failed-snapshot poll (re-enumeration inconclusive). `reads` may still
// carry attempted per-pid reads against previously known pids.
tsdev::CensusPoll badSnap(double t, std::vector<tsdev::CensusPidRead> reads) {
    tsdev::CensusPoll p;
    p.timeSecs = t;
    p.snapshotOk = false;
    p.reads = reads;
    return p;
}

// N polls of pid==lock==0 readings, step `step`, starting at t0.
void appendZeros(std::vector<tsdev::CensusPoll>& v, double t0, double step,
                 int n, DWORD pid) {
    for (int i = 0; i < n; ++i) {
        double t = t0 + step * i;
        v.push_back(okPoll(t, {pid}, {lockRead(pid, 0)}));
    }
}

const double kWindow = 3.0;
const double kBudget = 30.0;

// --- group-19 helpers: fabricated on-disk images --------------------------------

// Writes `bytes` to a fresh temp file and returns its path (empty on
// failure). The caller deletes it.
std::wstring writeTempImage(const std::vector<unsigned char>& bytes,
                            const char* tag) {
    wchar_t dir[MAX_PATH];
    if (GetTempPathW(MAX_PATH, dir) == 0) return L"";
    const std::wstring path = std::wstring(dir) + L"tsdev_census_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::wstring(tag, tag + std::strlen(tag)) + L".bin";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    DWORD w = 0;
    const BOOL ok = WriteFile(h, bytes.data(),
                              static_cast<DWORD>(bytes.size()), &w, nullptr);
    CloseHandle(h);
    if (!ok || w != bytes.size()) {
        DeleteFileW(path.c_str());
        return L"";
    }
    return path;
}

// Fabricated PE32 image whose header fields match the probed-build
// constants in whserver_census.h. tds/checkSum/size are parameters so the
// negative direction perturbs exactly one field at a time.
std::vector<unsigned char> fakeWhImage(DWORD tds, DWORD checkSum,
                                       size_t size) {
    std::vector<unsigned char> img(size, 0);
    const DWORD lfanew = 0x110;
    img[0] = 'M'; img[1] = 'Z';
    std::memcpy(&img[0x3C], &lfanew, 4);
    img[lfanew] = 'P'; img[lfanew + 1] = 'E';
    const WORD machine = 0x14C;   // IMAGE_FILE_MACHINE_I386
    std::memcpy(&img[lfanew + 4], &machine, 2);
    std::memcpy(&img[lfanew + 8], &tds, 4);          // TimeDateStamp
    const WORD magic = 0x10B;                        // PE32
    std::memcpy(&img[lfanew + 24], &magic, 2);
    std::memcpy(&img[lfanew + 24 + 64], &checkSum, 4);  // CheckSum
    return img;
}

int runTests() {
    using tsdev::CensusVerdict;

    // -------------------------------------------------------------------
    // Group 1: sustained lock==0 >= windowSecs => Pass.
    // -------------------------------------------------------------------
    {
        std::vector<tsdev::CensusPoll> s;
        appendZeros(s, 0.0, 0.25, 20, 100);  // zeros through t=4.75
        tsdev::CensusDecision d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Pass)
            return fail("g1: sustained zero did not Pass", 10);
        // Truncated stream (zeros only through t=2.75 < window) must NOT pass.
        std::vector<tsdev::CensusPoll> s2;
        appendZeros(s2, 0.0, 0.25, 12, 100);  // through t=2.75
        d = tsdev::decideCensus(s2, kWindow, kBudget);
        if (d.verdict == CensusVerdict::Pass)
            return fail("g1: sub-window zero stream Passed", 11);
        std::fprintf(stdout, "    PASS: group 1 (sustained zero => Pass)\n");
    }

    // -------------------------------------------------------------------
    // Group 2: lock>0 persists through the budget => Refuse.
    // -------------------------------------------------------------------
    {
        std::vector<tsdev::CensusPoll> s;
        for (int i = 0; i <= 40; ++i) {
            double t = 0.75 * i;  // through t=30
            s.push_back(okPoll(t, {100}, {lockRead(100, 6)}));
        }
        tsdev::CensusDecision d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Refuse)
            return fail("g2: lock>0 through budget did not Refuse", 20);
        if (d.pid != 100)
            return fail("g2: refuse did not report outstanding pid", 21);
        std::fprintf(stdout, "    PASS: group 2 (lock>0 at budget => Refuse)\n");
    }

    // -------------------------------------------------------------------
    // Group 3: transient openError on a STILL-LISTED pid => Indeterminate,
    // terminal (never evidence of absence, never downgrades).
    // -------------------------------------------------------------------
    {
        std::vector<tsdev::CensusPoll> s;
        appendZeros(s, 0.0, 0.25, 4, 100);  // zeros t=0..0.75
        // Pid still enumerated, OpenProcess transiently fails (ERROR_ACCESS_DENIED).
        s.push_back(okPoll(1.0, {100}, {errRead(100, ERROR_ACCESS_DENIED, 0)}));
        // More zeros after — must NOT rescue the verdict.
        appendZeros(s, 1.25, 0.25, 40, 100);
        tsdev::CensusDecision d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Indeterminate)
            return fail("g3: openError on listed pid not Indeterminate", 30);
        if (d.pid != 100 || d.error != ERROR_ACCESS_DENIED)
            return fail("g3: Indeterminate lost pid/error classification", 31);
        std::fprintf(stdout, "    PASS: group 3 (openError on listed => Indeterminate)\n");
    }

    // -------------------------------------------------------------------
    // Group 4: OpenProcess ok + RPM fails on a listed pid => Indeterminate.
    // -------------------------------------------------------------------
    {
        std::vector<tsdev::CensusPoll> s;
        s.push_back(okPoll(0.0, {100}, {lockRead(100, 0)}));
        s.push_back(okPoll(0.25, {100}, {errRead(100, 0, ERROR_PARTIAL_COPY)}));
        appendZeros(s, 0.5, 0.25, 40, 100);
        tsdev::CensusDecision d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Indeterminate)
            return fail("g4: rpmError on listed pid not Indeterminate", 40);
        if (d.error != ERROR_PARTIAL_COPY)
            return fail("g4: Indeterminate lost rpmError classification", 41);
        std::fprintf(stdout, "    PASS: group 4 (rpmError on listed => Indeterminate)\n");
    }

    // -------------------------------------------------------------------
    // Group 5: absent arm — pid absent from >=3 consecutive snapshotOk
    // re-enumerations => Pass; 2 absences is NOT enough.
    // -------------------------------------------------------------------
    {
        std::vector<tsdev::CensusPoll> s;
        s.push_back(okPoll(0.0, {100}, {lockRead(100, 0)}));
        s.push_back(okPoll(0.2, {}, {}));   // absent 1
        s.push_back(okPoll(0.4, {}, {}));   // absent 2
        tsdev::CensusDecision d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict == CensusVerdict::Pass)
            return fail("g5: 2 re-enumerated absences Passed", 50);
        s.push_back(okPoll(0.6, {}, {}));   // absent 3 => confirmed absent
        d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Pass)
            return fail("g5: 3 re-enumerated absences did not Pass", 51);
        std::fprintf(stdout, "    PASS: group 5 (absent arm >=3 re-enumerations)\n");
    }

    // -------------------------------------------------------------------
    // Group 6: failed snapshot is inconclusive — NEVER absent. A pid that
    // vanishes while re-enumeration keeps failing can never reach the
    // absent arm => Refuse at budget.
    // -------------------------------------------------------------------
    {
        std::vector<tsdev::CensusPoll> s;
        s.push_back(okPoll(0.0, {100}, {lockRead(100, 0)}));
        for (int i = 1; i <= 10; ++i) {
            s.push_back(badSnap(0.25 * i, {}));  // snapshots keep failing
        }
        tsdev::CensusDecision d =
            tsdev::decideCensus(s, kWindow, 2.0 /*budget*/);
        if (d.verdict != CensusVerdict::Refuse)
            return fail("g6: failed snapshots reached a non-Refuse verdict", 60);
        // A failed snapshot also BREAKS a consecutive-absent streak: absent,
        // absent, failed, absent — never 3 consecutive snapshotOk absences.
        std::vector<tsdev::CensusPoll> s2;
        s2.push_back(okPoll(0.0, {100}, {lockRead(100, 0)}));
        s2.push_back(okPoll(0.2, {}, {}));
        s2.push_back(okPoll(0.4, {}, {}));
        s2.push_back(badSnap(0.6, {}));
        s2.push_back(okPoll(0.8, {}, {}));
        s2.push_back(okPoll(1.0, {}, {}));   // streak only 2 so far
        d = tsdev::decideCensus(s2, kWindow, kBudget);
        if (d.verdict == CensusVerdict::Pass)
            return fail("g6: failed snapshot did not break absent streak", 61);
        s2.push_back(okPoll(1.2, {}, {}));   // 3rd consecutive => Pass
        d = tsdev::decideCensus(s2, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Pass)
            return fail("g6: rebuilt absent streak did not Pass", 62);
        std::fprintf(stdout, "    PASS: group 6 (failed snapshot inconclusive)\n");
    }

    // -------------------------------------------------------------------
    // Group 7: a pid APPEARING mid-window resets the sustained-zero window
    // for the whole census.
    // -------------------------------------------------------------------
    {
        std::vector<tsdev::CensusPoll> s;
        appendZeros(s, 0.0, 0.25, 8, 100);           // pid100 zeros t=0..1.75
        s.push_back(okPoll(2.0, {100, 200},
                           {lockRead(100, 0), lockRead(200, 0)}));  // pid200 appears
        for (int i = 1; i <= 11; ++i) {              // both zero t=2.25..4.75
            double t = 2.0 + 0.25 * i;
            s.push_back(okPoll(t, {100, 200},
                               {lockRead(100, 0), lockRead(200, 0)}));
        }
        // Through t=4.75 the reset window (epoch 2.0) has only run 2.75 s.
        tsdev::CensusDecision d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict == CensusVerdict::Pass)
            return fail("g7: census Passed before post-appearance window", 70);
        s.push_back(okPoll(5.0, {100, 200},
                           {lockRead(100, 0), lockRead(200, 0)}));
        d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Pass)
            return fail("g7: census did not Pass after reset window", 71);
        std::fprintf(stdout, "    PASS: group 7 (appearing pid resets window)\n");
    }

    // -------------------------------------------------------------------
    // Group 8: a pid VANISHING mid-window enters the absent arm — its stale
    // zero run is NOT evidence; it resolves only via re-enumerated absence.
    // -------------------------------------------------------------------
    {
        std::vector<tsdev::CensusPoll> s;
        appendZeros(s, 0.0, 0.25, 5, 100);   // pid100 zeros t=0..1.0 (< window)
        // pid100 vanishes. By t=1.4 its stale zero run is 1.4s old — but it
        // is NOT in the latest snapshot, so the run is no evidence; with
        // only 2 consecutive absences there must be no Pass.
        s.push_back(okPoll(1.2, {}, {}));
        s.push_back(okPoll(1.4, {}, {}));
        tsdev::CensusDecision d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict == CensusVerdict::Pass)
            return fail("g8: stale zero run Passed on vanished pid", 80);
        s.push_back(okPoll(1.6, {}, {}));  // 3rd consecutive absence => Pass
        d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Pass)
            return fail("g8: vanished pid absent-arm did not Pass", 81);
        // And a pid that reappears before its absence is confirmed resets
        // its absent streak (absent, absent, listed, absent x3 => Pass only
        // after the SECOND streak completes).
        std::vector<tsdev::CensusPoll> s2;
        s2.push_back(okPoll(0.0, {100}, {lockRead(100, 0)}));
        s2.push_back(okPoll(0.2, {}, {}));
        s2.push_back(okPoll(0.4, {}, {}));                        // streak 2
        s2.push_back(okPoll(0.6, {100}, {lockRead(100, 0)}));     // reappears
        s2.push_back(okPoll(0.8, {}, {}));
        s2.push_back(okPoll(1.0, {}, {}));                        // streak 2 again
        d = tsdev::decideCensus(s2, kWindow, kBudget);
        if (d.verdict == CensusVerdict::Pass)
            return fail("g8: reappeared pid kept its absent streak", 82);
        s2.push_back(okPoll(1.2, {}, {}));                        // streak 3
        d = tsdev::decideCensus(s2, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Pass)
            return fail("g8: rebuilt absent streak did not Pass", 83);
        std::fprintf(stdout, "    PASS: group 8 (vanished pid => absent arm only)\n");
    }

    // -------------------------------------------------------------------
    // Group 9: multi-pid — pass requires EVERY pid resolved; one held pid
    // blocks; a listed pid with no read outcome => Indeterminate.
    // -------------------------------------------------------------------
    {
        std::vector<tsdev::CensusPoll> s;
        for (int i = 0; i <= 12; ++i) {              // both zero through t=3.0
            double t = 0.25 * i;
            s.push_back(okPoll(t, {100, 200},
                               {lockRead(100, 0), lockRead(200, 0)}));
        }
        tsdev::CensusDecision d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Pass)
            return fail("g9: two zero pids did not Pass", 90);
        // One pid nonzero => Refuse.
        std::vector<tsdev::CensusPoll> s2;
        for (int i = 0; i <= 20; ++i) {
            double t = 0.5 * i;
            s2.push_back(okPoll(t, {100, 200},
                                {lockRead(100, 0), lockRead(200, 3)}));
        }
        d = tsdev::decideCensus(s2, kWindow, 10.0);
        if (d.verdict != CensusVerdict::Refuse)
            return fail("g9: one held pid did not Refuse", 91);
        if (d.pid != 200)
            return fail("g9: refuse did not report the held pid", 92);
        // Listed pid with no read => Indeterminate (unverifiable, fail closed).
        std::vector<tsdev::CensusPoll> s3;
        s3.push_back(okPoll(0.0, {100}, {noRead(100)}));
        d = tsdev::decideCensus(s3, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Indeterminate)
            return fail("g9: listed pid with no read not Indeterminate", 93);
        std::fprintf(stdout, "    PASS: group 9 (multi-pid / no-read discipline)\n");
    }

    // -------------------------------------------------------------------
    // Group 10: never-any-pids census — absent at census start passes only
    // under the same >=3 consecutive successful empty re-enumerations.
    // -------------------------------------------------------------------
    {
        std::vector<tsdev::CensusPoll> s;
        s.push_back(okPoll(0.0, {}, {}));
        s.push_back(okPoll(0.2, {}, {}));
        tsdev::CensusDecision d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict == CensusVerdict::Pass)
            return fail("g10: 2 empty snapshots Passed at census start", 100);
        s.push_back(okPoll(0.4, {}, {}));
        d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Pass)
            return fail("g10: 3 empty snapshots did not Pass", 101);
        std::fprintf(stdout, "    PASS: group 10 (absent-at-start discipline)\n");
    }

    // -------------------------------------------------------------------
    // Group 11: REAL readWhLockSample — OpenProcess self + ReadProcessMemory
    // of a known static at its own VA (zero TS contact). Also: invalid pid
    // fails with a classified openError, and the snapshotOk variant of
    // findWhServerPids distinguishes success from failure.
    // -------------------------------------------------------------------
    {
        tsdev::CensusTarget self;
        self.pid = GetCurrentProcessId();
        self.lockVa = reinterpret_cast<uintptr_t>(&g_lockSim);
        self.activityVa = reinterpret_cast<uintptr_t>(&g_actSim);
        tsdev::WhLockSample smp;
        bool ok = tsdev::readWhLockSample(self, smp);
        std::fprintf(stdout,
            "  [g11] self RPM: ok=%d lock=%lu openErr=%lu rpmErr=%lu act=%u actValid=%d\n",
            ok ? 1 : 0, smp.lockValue, smp.openError, smp.rpmError,
            (unsigned)smp.activity, smp.activityValid ? 1 : 0);
        if (!ok) return fail("readWhLockSample(self) returned false", 110);
        if (smp.lockValue != 7)
            return fail("readWhLockSample read wrong lock value", 111);
        if (!smp.activityValid || smp.activity != 1)
            return fail("readWhLockSample activity read failed", 112);

        tsdev::CensusTarget bad{0xFFFFFFFF, 0x1000, 0};
        tsdev::WhLockSample smp2;
        if (tsdev::readWhLockSample(bad, smp2))
            return fail("readWhLockSample(invalid pid) returned true", 113);
        if (smp2.openError == 0)
            return fail("readWhLockSample(invalid pid) lost openError", 114);

        std::vector<DWORD> pids;
        if (!tsdev::findWhServerPids(pids))
            return fail("findWhServerPids(snapshotOk) reported failure", 115);
        std::fprintf(stdout, "    PASS: group 11 (real RPM + snapshotOk variant)\n");
    }

    // -------------------------------------------------------------------
    // Group 12: a zero read on a FAILED-snapshot poll may extend a zero
    // run (continuous lock evidence), but the Pass moment itself requires
    // a fresh successful re-enumeration — a run reaching the window inside
    // a snapshot-failure gap must NOT complete the census (the pid set is
    // unknowable there). Pass only on a later snapshotOk poll, and the
    // Pass decision must carry pid=0/error=0 (not a stale outstanding).
    // -------------------------------------------------------------------
    {
        std::vector<tsdev::CensusPoll> s;
        appendZeros(s, 0.0, 0.25, 12, 100);   // zeros t=0..2.75 (< window)
        // Failed snapshot at t=3.0 still carries a valid zero read: the
        // run extends past the window — but Pass is forbidden here.
        s.push_back(badSnap(3.0, {lockRead(100, 0)}));
        tsdev::CensusDecision d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict == CensusVerdict::Pass)
            return fail("g12: Pass completed on a failed-snapshot poll", 120);
        if (d.verdict != CensusVerdict::Refuse)
            return fail("g12: gap-extended stream did not fail closed", 121);
        // A later successful re-enumeration completes the census, and the
        // Pass resets the previously reported outstanding pid.
        s.push_back(okPoll(3.25, {100}, {lockRead(100, 0)}));
        d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Pass)
            return fail("g12: no Pass on later successful snapshot", 122);
        if (d.pid != 0 || d.error != 0)
            return fail("g12: Pass did not reset pid/error fields", 123);
        std::fprintf(stdout, "    PASS: group 12 (Pass gated on snapshotOk)\n");
    }

    // -------------------------------------------------------------------
    // Group 13: an absentConfirmed pid REAPPEARING un-confirms it — its
    // post-reappearance reads are live evidence again, so a lock>0 read
    // blocks the census (had it stayed 'absent', the others would Pass).
    // -------------------------------------------------------------------
    {
        std::vector<tsdev::CensusPoll> s;
        s.push_back(okPoll(0.0, {100, 200},
                           {lockRead(100, 0), lockRead(200, 0)}));
        for (int i = 1; i <= 3; ++i) {       // 100 absent x3 => confirmed
            s.push_back(okPoll(0.2 * i, {200}, {lockRead(200, 0)}));
        }
        // 100 reappears holding a lock — it must count as live evidence.
        s.push_back(okPoll(0.8, {100, 200},
                           {lockRead(100, 1), lockRead(200, 0)}));
        for (int i = 1; i <= 30; ++i) {      // through budget, 100 held
            double t = 0.8 + 0.25 * i;
            s.push_back(okPoll(t, {100, 200},
                               {lockRead(100, 1), lockRead(200, 0)}));
        }
        tsdev::CensusDecision d =
            tsdev::decideCensus(s, kWindow, 6.0 /*budget*/);
        if (d.verdict != CensusVerdict::Refuse)
            return fail("g13: reappeared pid stayed confirmed-absent", 130);
        if (d.pid != 100)
            return fail("g13: refuse did not report the reappeared pid", 131);
        std::fprintf(stdout, "    PASS: group 13 (reappearance un-confirms)\n");
    }

    // -------------------------------------------------------------------
    // Group 14: a listed pid with NO read entry at all in the poll's read
    // vector (read==nullptr, not merely attempted==false) => Indeterminate.
    // -------------------------------------------------------------------
    {
        std::vector<tsdev::CensusPoll> s;
        s.push_back(okPoll(0.0, {100}, {}));  // listed, zero read entries
        tsdev::CensusDecision d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Indeterminate)
            return fail("g14: listed pid with no read entry not Indeterminate", 140);
        if (d.pid != 100 || d.error != 0)
            return fail("g14: Indeterminate misclassified missing read", 141);
        // Reads exist but none for the listed pid — same fail-closed path.
        std::vector<tsdev::CensusPoll> s2;
        s2.push_back(okPoll(0.0, {100}, {lockRead(200, 0)}));
        d = tsdev::decideCensus(s2, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Indeterminate)
            return fail("g14: read for another pid covered the listed pid", 142);
        std::fprintf(stdout, "    PASS: group 14 (no read entry => Indeterminate)\n");
    }

    // -------------------------------------------------------------------
    // Group 15: mixed multi-pid Pass — one pid resolved via the
    // absentConfirmed arm while another resolves via a listed sustained
    // zero run; both arms compose in the same Pass evaluation.
    // -------------------------------------------------------------------
    {
        std::vector<tsdev::CensusPoll> s;
        s.push_back(okPoll(0.0, {100, 200},
                           {lockRead(100, 0), lockRead(200, 0)}));
        for (int i = 1; i <= 3; ++i) {       // 100 absent x3 => confirmed
            s.push_back(okPoll(0.2 * i, {200}, {lockRead(200, 0)}));
        }
        // 200 keeps its unbroken zero run from t=0; window met at t=3.0.
        for (int i = 1; i <= 11; ++i) {
            double t = 0.6 + 0.25 * i;
            s.push_back(okPoll(t, {200}, {lockRead(200, 0)}));
        }
        tsdev::CensusDecision d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Pass)
            return fail("g15: absent+zero mixed census did not Pass", 150);
        if (d.pid != 0 || d.error != 0)
            return fail("g15: Pass did not reset pid/error fields", 151);
        std::fprintf(stdout, "    PASS: group 15 (mixed absent+zero Pass)\n");
    }

    // -------------------------------------------------------------------
    // Group 16: degenerate inputs fail closed — an empty poll stream
    // Refuses, and a non-positive windowSecs Refuses even on a sustained
    // zero stream (invalid window must never fail the gate open).
    // -------------------------------------------------------------------
    {
        std::vector<tsdev::CensusPoll> empty;
        tsdev::CensusDecision d = tsdev::decideCensus(empty, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Refuse)
            return fail("g16: empty poll stream did not Refuse", 160);
        if (d.pid != 0 || d.error != 0)
            return fail("g16: empty stream Refuse carried stray fields", 161);
        std::vector<tsdev::CensusPoll> s;
        appendZeros(s, 0.0, 0.25, 20, 100);
        d = tsdev::decideCensus(s, 0.0 /*window*/, kBudget);
        if (d.verdict == CensusVerdict::Pass)
            return fail("g16: windowSecs==0 failed open on zero stream", 162);
        d = tsdev::decideCensus(s, -1.0 /*window*/, kBudget);
        if (d.verdict == CensusVerdict::Pass)
            return fail("g16: windowSecs<0 failed open on zero stream", 163);
        std::fprintf(stdout, "    PASS: group 16 (degenerate inputs refuse)\n");
    }

    // -------------------------------------------------------------------
    // Group 17: a pid appearing AND vanishing still resets the whole-census
    // zero window — the appearing-pid epoch reset is not undone by the pid
    // later reaching absentConfirmed. pid200 appears at t=3.0, reads zero,
    // vanishes, and is absentConfirmed by t=3.75; pid100 must nevertheless
    // re-accumulate its sustained-zero run from the t=3.0 epoch (no Pass
    // at t=4.0; Pass only at/after t=6.0).
    // -------------------------------------------------------------------
    {
        std::vector<tsdev::CensusPoll> s;
        appendZeros(s, 0.0, 0.25, 12, 100);  // pid100 zeros t=0..2.75
        // pid200 appears on a successful re-enumeration, reading zero.
        s.push_back(okPoll(3.0, {100, 200},
                           {lockRead(100, 0), lockRead(200, 0)}));
        // pid200 vanishes: 3 consecutive snapshotOk enumerations listing
        // only pid100 => absentConfirmed at t=3.75.
        s.push_back(okPoll(3.25, {100}, {lockRead(100, 0)}));
        s.push_back(okPoll(3.5,  {100}, {lockRead(100, 0)}));
        s.push_back(okPoll(3.75, {100}, {lockRead(100, 0)}));
        s.push_back(okPoll(4.0,  {100}, {lockRead(100, 0)}));
        tsdev::CensusDecision d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict == CensusVerdict::Pass)
            return fail("g17: appearing-then-vanished pid did not reset window", 170);
        // From the t=3.0 epoch, pid100's window completes at t=6.0.
        s.push_back(okPoll(4.5, {100}, {lockRead(100, 0)}));
        s.push_back(okPoll(5.0, {100}, {lockRead(100, 0)}));
        s.push_back(okPoll(5.5, {100}, {lockRead(100, 0)}));
        d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict == CensusVerdict::Pass)
            return fail("g17: census Passed before epoch+window (t<6.0)", 171);
        s.push_back(okPoll(6.0, {100}, {lockRead(100, 0)}));
        d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Pass)
            return fail("g17: census did not Pass at epoch+window (t=6.0)", 172);
        std::fprintf(stdout, "    PASS: group 17 (appearing+vanished pid still resets)\n");
    }

    // -------------------------------------------------------------------
    // Group 18: image-identity gate (R5) — a listed pid whose image is not
    // the probed WHServer build produces an attempted read with
    // imageKnown=false and a ZEROED sample. That persistent 0 is the exact
    // false-pass the gate exists to prevent => Indeterminate with
    // ERROR_INVALID_IMAGE_HASH, terminal, never a Pass.
    // -------------------------------------------------------------------
    {
        // First-poll unknown image => immediate Indeterminate, classified.
        std::vector<tsdev::CensusPoll> s;
        s.push_back(okPoll(0.0, {100}, {unknownImageRead(100)}));
        tsdev::CensusDecision d = tsdev::decideCensus(s, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Indeterminate)
            return fail("g18: unknown image not Indeterminate", 180);
        if (d.pid != 100 || d.error != ERROR_INVALID_IMAGE_HASH)
            return fail("g18: Indeterminate lost image-hash classification", 181);

        // Unknown image appearing mid-window after known zeros => still
        // Indeterminate (the later read is unverifiable).
        std::vector<tsdev::CensusPoll> s2;
        appendZeros(s2, 0.0, 0.25, 4, 100);
        s2.push_back(okPoll(1.0, {100}, {unknownImageRead(100)}));
        appendZeros(s2, 1.25, 0.25, 40, 100);
        d = tsdev::decideCensus(s2, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Indeterminate)
            return fail("g18: mid-window unknown image not Indeterminate", 182);

        // Multi-pid: one unknown image blocks the whole census even while
        // every other pid reads a genuine sustained zero.
        std::vector<tsdev::CensusPoll> s3;
        s3.push_back(okPoll(0.0, {100, 200},
                            {lockRead(100, 0), unknownImageRead(200)}));
        d = tsdev::decideCensus(s3, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Indeterminate || d.pid != 200)
            return fail("g18: one unknown image did not block census", 183);

        // A sustained zero stream that NEVER passes the gate: every poll
        // reads lock==0 on an unprobed image — must refuse, never pass.
        std::vector<tsdev::CensusPoll> s4;
        for (int i = 0; i <= 40; ++i) {
            double t = 0.75 * i;  // through budget t=30
            s4.push_back(okPoll(t, {100}, {unknownImageRead(100)}));
        }
        d = tsdev::decideCensus(s4, kWindow, kBudget);
        if (d.verdict == CensusVerdict::Pass)
            return fail("g18: persistent unknown-image zero Passed", 184);

        // An imageKnown=false zero read inside a failed-snapshot gap must
        // NOT extend a zero run — the run breaks, so the post-gap window
        // restarts at the next snapshotOk poll.
        std::vector<tsdev::CensusPoll> s5;
        appendZeros(s5, 0.0, 0.25, 12, 100);   // zeros t=0..2.75
        s5.push_back(badSnap(3.0, {unknownImageRead(100)}));
        s5.push_back(okPoll(3.25, {100}, {lockRead(100, 0)}));
        d = tsdev::decideCensus(s5, kWindow, kBudget);
        if (d.verdict == CensusVerdict::Pass)
            return fail("g18: unknown-image gap read extended zero run", 185);
        appendZeros(s5, 3.5, 0.25, 10, 100);   // zeros t=3.5..5.75
        d = tsdev::decideCensus(s5, kWindow, kBudget);
        if (d.verdict == CensusVerdict::Pass)
            return fail("g18: zero run passed before post-gap window", 186);
        s5.push_back(okPoll(6.25, {100}, {lockRead(100, 0)}));
        d = tsdev::decideCensus(s5, kWindow, kBudget);
        if (d.verdict != CensusVerdict::Pass)
            return fail("g18: census did not Pass at post-gap window", 187);
        std::fprintf(stdout, "    PASS: group 18 (image-identity gate)\n");
    }

    // -------------------------------------------------------------------
    // Group 19: whServerImageIsKnown / whServerPidImageIsKnown against
    // fabricated files — every failure direction fails closed. Runs on
    // machines WITHOUT TradeStation: no installed WHServer is assumed.
    // -------------------------------------------------------------------
    {
        if (tsdev::whServerImageIsKnown(
                L"C:\\path\\that\\does\\not\\exist\\WHServer.exe"))
            return fail("g19: nonexistent path reported known", 190);

        // A real but unrelated PE (this test exe) => not known.
        wchar_t ownExe[MAX_PATH];
        if (GetModuleFileNameW(nullptr, ownExe, MAX_PATH) == 0)
            return fail("g19: GetModuleFileNameW(self) failed", 191);
        if (tsdev::whServerImageIsKnown(ownExe))
            return fail("g19: own exe reported known", 192);

        // A fabricated image matching every probed-build constant =>
        // known. Single-field perturbations => not known.
        const std::wstring match = writeTempImage(
            fakeWhImage(tsdev::kWhServerTimeStamp, tsdev::kWhServerCheckSum,
                        static_cast<size_t>(tsdev::kWhServerImageSize)),
            "match");
        if (match.empty())
            return fail("g19: temp image write failed", 193);
        if (!tsdev::whServerImageIsKnown(match)) {
            DeleteFileW(match.c_str());
            return fail("g19: fabricated probed-build image not known", 194);
        }
        DeleteFileW(match.c_str());

        const std::wstring badTds = writeTempImage(
            fakeWhImage(tsdev::kWhServerTimeStamp + 1,
                        tsdev::kWhServerCheckSum,
                        static_cast<size_t>(tsdev::kWhServerImageSize)),
            "badtds");
        const std::wstring badCks = writeTempImage(
            fakeWhImage(tsdev::kWhServerTimeStamp,
                        tsdev::kWhServerCheckSum + 1,
                        static_cast<size_t>(tsdev::kWhServerImageSize)),
            "badcks");
        const std::wstring badSize = writeTempImage(
            fakeWhImage(tsdev::kWhServerTimeStamp, tsdev::kWhServerCheckSum,
                        static_cast<size_t>(tsdev::kWhServerImageSize) - 1),
            "badsize");
        if (badTds.empty() || badCks.empty() || badSize.empty())
            return fail("g19: perturbed temp image write failed", 195);
        const bool perturbedKnown =
            tsdev::whServerImageIsKnown(badTds) ||
            tsdev::whServerImageIsKnown(badCks) ||
            tsdev::whServerImageIsKnown(badSize);
        DeleteFileW(badTds.c_str());
        DeleteFileW(badCks.c_str());
        DeleteFileW(badSize.c_str());
        if (perturbedKnown)
            return fail("g19: perturbed image reported known", 196);

        // Right size, non-PE garbage => not known.
        std::vector<unsigned char> junk(
            static_cast<size_t>(tsdev::kWhServerImageSize), 0xA5);
        const std::wstring junkPath = writeTempImage(junk, "junk");
        if (junkPath.empty())
            return fail("g19: junk temp image write failed", 197);
        const bool junkKnown = tsdev::whServerImageIsKnown(junkPath);
        DeleteFileW(junkPath.c_str());
        if (junkKnown)
            return fail("g19: non-PE junk reported known", 198);

        // PID-level wrapper: own process is not the WHServer build =>
        // not known; an invalid pid cannot even resolve an image.
        if (tsdev::whServerPidImageIsKnown(GetCurrentProcessId()))
            return fail("g19: own pid image reported known", 199);
        if (tsdev::whServerPidImageIsKnown(0xFFFFFFFF))
            return fail("g19: invalid pid reported known", 200);
        std::fprintf(stdout, "    PASS: group 19 (binary-identity predicate)\n");
    }

    // -------------------------------------------------------------------
    // Group 20: whServerHasForeignClients -- the pre-activation latch's
    // pure evaluation. All three seams are scripted; every inconclusive
    // direction must latch FALSE (the conservative outcome -- the recycle
    // attempt then proceeds and the fail-closed census stays the
    // authority).
    // -------------------------------------------------------------------
    {
        using tsdev::WhLockSample;
        auto locks = [](DWORD lock) {
            return [lock](DWORD, WhLockSample& s) -> bool {
                s.lockValue = lock;
                return true;
            };
        };
        auto readFails = [](DWORD, WhLockSample&) -> bool { return false; };
        auto allKnown = [](DWORD) -> bool { return true; };
        auto noneKnown = [](DWORD) -> bool { return false; };
        auto pids = [](std::vector<DWORD> v) {
            return [v](std::vector<DWORD>& out) -> bool {
                out = v;
                return true;
            };
        };
        auto enumFails = [](std::vector<DWORD>& out) -> bool {
            out.clear();
            return false;
        };

        // No WHServer running at all -> no foreign clients.
        if (tsdev::whServerHasForeignClients(
                pids({}), allKnown, locks(7)))
            return fail("g20: empty pid set latched foreign", 201);
        // Inconclusive enumeration -> false (never evidence either way).
        if (tsdev::whServerHasForeignClients(
                enumFails, allKnown, locks(7)))
            return fail("g20: failed enumeration latched foreign", 202);
        // lock==0 on every known pid -> false (server free of clients).
        if (tsdev::whServerHasForeignClients(
                pids({100, 200}), allKnown, locks(0)))
            return fail("g20: all-zero locks latched foreign", 203);
        // Any positive lock -> true (foreign client present).
        if (!tsdev::whServerHasForeignClients(
                pids({100}), allKnown, locks(7)))
            return fail("g20: lock==7 not latched foreign", 204);
        // Short-circuit ordering: a zero-lock first pid does not hide a
        // positive-lock second pid.
        {
            int reads = 0;
            auto seqLocks = [&](DWORD, WhLockSample& s) -> bool {
                ++reads;
                s.lockValue = (reads == 1) ? 0 : 3;
                return true;
            };
            if (!tsdev::whServerHasForeignClients(
                    pids({100, 200}), allKnown, seqLocks))
                return fail("g20: second-pid lock>0 not latched", 205);
            // And once a positive lock is seen, no further pids are probed.
            reads = 0;
            auto countLocks = [&](DWORD, WhLockSample& s) -> bool {
                ++reads;
                s.lockValue = 5;
                return true;
            };
            if (!tsdev::whServerHasForeignClients(
                    pids({100, 200}), allKnown, countLocks))
                return fail("g20: positive lock not latched (count)", 206);
            if (reads != 1)
                return fail("g20: latch did not short-circuit after lock>0",
                            207);
        }
        // Unprobed image -> the pid's bytes are no evidence; skipped.
        if (tsdev::whServerHasForeignClients(
                pids({100}), noneKnown, locks(7)))
            return fail("g20: unknown-image lock>0 latched foreign", 208);
        // A failed read is skipped; a later positive lock still latches.
        {
            int calls = 0;
            auto flaky = [&](DWORD pid, WhLockSample& s) -> bool {
                ++calls;
                if (pid == 100) return false;   // unreadable
                s.lockValue = 9;
                return true;
            };
            if (!tsdev::whServerHasForeignClients(
                    pids({100, 200}), allKnown, flaky))
                return fail("g20: failed read hid a later lock>0", 209);
        }
        // All reads fail -> false (conservative).
        if (tsdev::whServerHasForeignClients(
                pids({100, 200}), allKnown, readFails))
            return fail("g20: all-failed reads latched foreign", 210);
        std::fprintf(stdout,
            "    PASS: group 20 (foreign-client latch evaluation)\n");
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

} // namespace

int main() {
    try {
        int ec = runTests();
        std::fprintf(stdout,
                     "\n=== test_whserver_census complete (exit %d) ===\n", ec);
        if (ec == 0) std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
        else         std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
        return ec;
    } catch (...) {
        std::fprintf(stderr, "\n[FATAL] unhandled exception\n");
        return 99;
    }
}
