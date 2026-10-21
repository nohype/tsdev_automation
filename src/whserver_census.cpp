// whserver_census.cpp
//
// Implementation of the WHServer m_nLock census declared in
// whserver_census.h. decideCensus is a pure function over an injected
// sample stream (the fail-closed gate of tmp/RECYCLE_ENGINE_DESIGN.md
// §3.3/§3.4); readWhLockSample is the thin OpenProcess+RPM seam.
// Pure Win32 (kernel32); see the header for the pinned contract.

#include "whserver_census.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace tsdev {

namespace {

// Reads exactly `n` bytes at file offset `off` of an open image file.
bool readImageAt(HANDLE h, ULONGLONG off, void* buf, DWORD n) {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(off);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(h, buf, n, &got, nullptr) && got == n;
}

} // namespace

bool whServerImageIsKnown(const std::wstring& imagePath) {
    HANDLE h = CreateFileW(imagePath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool known = false;
    LARGE_INTEGER size;
    size.QuadPart = 0;
    if (GetFileSizeEx(h, &size) &&
        size.QuadPart == static_cast<LONGLONG>(kWhServerImageSize)) {
        unsigned char dos[0x40];
        DWORD lfanew = 0;
        if (readImageAt(h, 0, dos, sizeof(dos)) &&
            dos[0] == 'M' && dos[1] == 'Z') {
            std::memcpy(&lfanew, dos + 0x3C, 4);
        }
        // PE\0\0 + IMAGE_FILE_HEADER(20) + optional header through the
        // CheckSum field (offset +64, shared by PE32 and PE32+).
        unsigned char nt[4 + 20 + 68];
        if (lfanew >= sizeof(dos) &&
            lfanew + sizeof(nt) <= size.QuadPart &&
            readImageAt(h, lfanew, nt, sizeof(nt)) &&
            nt[0] == 'P' && nt[1] == 'E' && nt[2] == 0 && nt[3] == 0) {
            WORD magic = 0;
            DWORD tds = 0, checkSum = 0;
            std::memcpy(&tds, nt + 4 + 4, 4);            // TimeDateStamp
            std::memcpy(&magic, nt + 4 + 20, 2);
            std::memcpy(&checkSum, nt + 4 + 20 + 64, 4); // CheckSum
            known = (magic == 0x10B || magic == 0x20B) &&
                    tds == kWhServerTimeStamp &&
                    checkSum == kWhServerCheckSum;
        }
    }
    CloseHandle(h);
    return known;
}

bool whServerPidImageIsKnown(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return false;
    wchar_t path[4096];
    DWORD n = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    const BOOL ok = QueryFullProcessImageNameW(h, 0, path, &n);
    CloseHandle(h);
    if (!ok) return false;
    return whServerImageIsKnown(path);
}

// Per-pid census state accumulated across polls.
struct CensusPidState {
    bool   absentConfirmed;  // reached >=3 consecutive snapshotOk absences
    int    absentStreak;     // consecutive snapshotOk polls not listing it
    bool   zeroValid;        // currently on an unbroken run of lock==0 reads
    double zeroSince;        // timeSecs at which the current zero run began
};

CensusDecision decideCensus(const std::vector<CensusPoll>& polls,
                            double windowSecs,
                            double budgetSecs) {
    CensusDecision dec{CensusVerdict::Refuse, 0, 0};

    // windowSecs <= 0 is invalid input: a non-positive sustained-zero
    // window would let a single zero poll satisfy "sustained >= window"
    // and fail the gate OPEN. Refuse instead (fail closed, §3.3).
    if (windowSecs <= 0.0) return dec;

    std::map<DWORD, CensusPidState> state;
    int    emptyStreak   = 0;       // consecutive snapshotOk polls listing 0 pids
    std::vector<DWORD> latestPids;  // pid set of the latest snapshotOk poll
    double epoch         = 0.0;     // last window-reset time (pid appearing)
    double t0            = 0.0;
    bool   haveT0        = false;

    for (const CensusPoll& poll : polls) {
        if (!haveT0) { t0 = poll.timeSecs; haveT0 = true; }

        if (!poll.snapshotOk) {
            // Failed re-enumeration: inconclusive, NEVER absent and never a
            // listed-pid error judgement (there is no listing). It breaks
            // every consecutive-absent streak, and any pid that produced no
            // valid zero read this poll loses its zero run (a gap in
            // evidence can never count toward a sustained zero).
            emptyStreak = 0;
            for (auto& kv : state) {
                CensusPidState& s = kv.second;
                s.absentStreak = 0;
                if (!s.absentConfirmed) {
                    bool zeroHere = false;
                    for (const CensusPidRead& r : poll.reads) {
                        if (r.pid == kv.first && r.attempted &&
                            r.imageKnown &&
                            r.sample.openError == 0 &&
                            r.sample.rpmError == 0 &&
                            r.sample.lockValue == 0) {
                            zeroHere = true;
                            break;
                        }
                    }
                    if (!zeroHere) s.zeroValid = false;
                }
            }
        } else {
            latestPids = poll.pids;
            if (poll.pids.empty()) ++emptyStreak; else emptyStreak = 0;

            // A pid APPEARING (never seen, or reappearing after a confirmed
            // absence) resets the sustained-zero window for the WHOLE
            // census (design §3.4): every run must re-accumulate windowSecs
            // measured from this poll.
            bool appeared = false;
            for (DWORD p : poll.pids) {
                auto it = state.find(p);
                if (it == state.end()) {
                    state[p] = CensusPidState{false, 0, false, 0.0};
                    appeared = true;
                } else if (it->second.absentConfirmed) {
                    it->second.absentConfirmed = false;
                    it->second.zeroValid = false;
                    appeared = true;
                }
            }
            if (appeared) epoch = poll.timeSecs;

            for (auto& kv : state) {
                CensusPidState& s = kv.second;
                const bool listed =
                    std::find(poll.pids.begin(), poll.pids.end(), kv.first) !=
                    poll.pids.end();
                if (s.absentConfirmed) continue;  // stays absent unless it reappears
                if (listed) {
                    s.absentStreak = 0;
                } else {
                    ++s.absentStreak;
                    if (s.absentStreak >= 3) s.absentConfirmed = true;
                    continue;
                }

                // Listed pid: ANY openError/rpmError on a still-listed pid
                // is Indeterminate — an error is never evidence of absence
                // and never evidence of zero. A listed pid with no read at
                // all is equally unverifiable: fail closed the same way.
                const CensusPidRead* read = nullptr;
                for (const CensusPidRead& r : poll.reads) {
                    if (r.pid == kv.first) { read = &r; break; }
                }
                if (read == nullptr || !read->attempted) {
                    dec.verdict = CensusVerdict::Indeterminate;
                    dec.pid = kv.first;
                    dec.error = 0;  // listed pid produced no read outcome
                    return dec;
                }
                // Image gate: an attempted read on an unprobed WHServer
                // build is untrusted — the fixed VAs are single-build
                // layout facts and can still map while reading a
                // persistent 0. Same fail-closed class as an openError.
                if (!read->imageKnown) {
                    dec.verdict = CensusVerdict::Indeterminate;
                    dec.pid = kv.first;
                    dec.error = ERROR_INVALID_IMAGE_HASH;
                    return dec;
                }
                if (read->sample.openError != 0 ||
                    read->sample.rpmError != 0) {
                    dec.verdict = CensusVerdict::Indeterminate;
                    dec.pid = kv.first;
                    dec.error = read->sample.openError != 0
                                    ? read->sample.openError
                                    : read->sample.rpmError;
                    return dec;
                }
                if (read->sample.lockValue == 0) {
                    if (!s.zeroValid) { s.zeroValid = true; s.zeroSince = poll.timeSecs; }
                } else {
                    s.zeroValid = false;
                }
            }
        }

        // Pass evaluation (design §3.4): every ever-known pid is either
        // confirmed absent or, in the latest snapshotOk set, on a
        // sustained-zero run >=windowSecs measured from the last
        // appearance epoch. A never-any-pids census needs the same >=3
        // consecutive successful empty re-enumerations (absent-at-start is
        // already recycled-equivalent, granted only under the same
        // error-code discipline).
        // Pass may complete ONLY on a poll whose fresh re-enumeration
        // succeeded: on a !snapshotOk poll the current pid set is
        // unknowable (an unenumerated pid could hold foreign locks), so
        // a zero run reaching the window inside a snapshot-failure gap
        // must never complete the census.
        if (poll.snapshotOk) {
            bool allResolved = true;
            DWORD outstanding = 0;
            for (const auto& kv : state) {
                const CensusPidState& s = kv.second;
                if (s.absentConfirmed) continue;
                // A non-absent pid resolves ONLY through a sustained-zero
                // run inside the latest snapshotOk set — a vanished pid
                // with a stale zero run is not evidence of anything until
                // its absence is re-enumeration-confirmed.
                const bool listed =
                    std::find(latestPids.begin(), latestPids.end(),
                              kv.first) != latestPids.end();
                const double effectiveStart =
                    s.zeroSince > epoch ? s.zeroSince : epoch;
                if (!(listed && s.zeroValid &&
                      poll.timeSecs - effectiveStart >= windowSecs)) {
                    allResolved = false;
                    outstanding = kv.first;
                    break;
                }
            }
            if (allResolved &&
                (state.empty() ? emptyStreak >= 3 : true)) {
                dec.verdict = CensusVerdict::Pass;
                dec.pid = 0;      // header contract: fields are 0 when
                dec.error = 0;    // not applicable (outstanding may be set)
                return dec;
            }
            dec.pid = outstanding;
        }

        // Budget elapsed without a verdict: refuse — lock>0 (or
        // unverifiable state) at budget end means foreign client-held
        // objects (or rundown lag; conservative direction only, §3.3).
        if (poll.timeSecs - t0 >= budgetSecs) {
            dec.verdict = CensusVerdict::Refuse;
            return dec;
        }
    }

    // Stream exhausted with no verdict: inconclusive => fail closed.
    dec.verdict = CensusVerdict::Refuse;
    return dec;
}

bool readWhLockSample(const CensusTarget& target, WhLockSample& out) {
    out = WhLockSample{};

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
                               PROCESS_VM_READ, FALSE, target.pid);
    if (hProc == nullptr) {
        out.openError = GetLastError();
        return false;
    }

    SIZE_T n = 0;
    if (!ReadProcessMemory(hProc, reinterpret_cast<LPCVOID>(target.lockVa),
                           &out.lockValue, sizeof(out.lockValue), &n) ||
        n != sizeof(out.lockValue)) {
        out.rpmError = GetLastError();
        CloseHandle(hProc);
        return false;
    }

    // m_bActivity: corroborating only — a failed read never fails the sample.
    if (target.activityVa != 0) {
        uint8_t b = 0;
        SIZE_T m = 0;
        if (ReadProcessMemory(hProc,
                              reinterpret_cast<LPCVOID>(target.activityVa),
                              &b, sizeof(b), &m) && m == sizeof(b)) {
            out.activity = b;
            out.activityValid = true;
        }
    }

    CloseHandle(hProc);
    return true;
}

} // namespace tsdev
