// whserver_census.h
//
// WHServer m_nLock census — the fail-closed safety gate that must confirm
// zero client-held objects (or a verified-absent server) before the recycle
// engine (tmp/RECYCLE_ENGINE_DESIGN.md §2 step 3, §3) may invoke
// IWHouse::ForceShutDown.
//
// Two-level seam (design §3.4):
//   level 1 (pure):  decideCensus() — gate decision over an injected stream
//                    of per-poll samples; no Win32 process access, fully
//                    unit-testable under -NoTs.
//   level 2 (thin):  readWhLockSample() — the real OpenProcess +
//                    ReadProcessMemory read of m_nLock / m_bActivity at
//                    fixed 32-bit VAs (no ASLR — PROVEN, KB §20).
//
// Contract (pinned in design §3.4, implemented verbatim by decideCensus):
//   * Each poll carries a FRESH re-enumerated pid set + a snapshotOk flag,
//     plus the per-pid read outcome (lock value | openError | rpmError).
//   * Absent arm: a pid counts absent ONLY after >=3 consecutive snapshotOk
//     re-enumerations that do not list it (mirrors the watchdog's
//     kWhServerGoneConfirmSamples — a single empty poll is inconclusive;
//     a FAILED snapshot is inconclusive, NEVER absent — snapshot-failure
//     ambiguity is a PROVEN code fact).
//   * Indeterminate: ANY openError/rpmError on a pid still listed in that
//     poll's snapshot => Indeterminate. A transient OpenProcess failure on
//     a live pid is NEVER evidence of absence (no "OpenProcess fails =>
//     absent" arm exists). Indeterminate NEVER downgrades to Pass; at
//     engine level Indeterminate => refuse (exit 19).
//   * Image gate: a listed pid whose attempted read carries
//     imageKnown=false => Indeterminate (error ERROR_INVALID_IMAGE_HASH).
//     The fixed VAs below are layout facts of ONE probed WHServer.exe
//     build; on an unprobed image the same VA can still map and read a
//     persistent 0, so those bytes are never evidence of anything.
//   * Mid-window pid-set changes: a pid APPEARING resets the sustained-zero
//     window for the whole census; a pid VANISHING enters the absent arm
//     (re-enumerated absence only).
//   * Pass: every pid in the latest snapshotOk snapshot read lock==0
//     sustained >=windowSecs, OR every pid reached re-enumerated absence.
//     Otherwise the census polls until the budget elapses => Refuse
//     (lock>0 at budget end = foreign client-held objects).
//
// The TU is pure Win32 (kernel32 only): no orlib20, no warehouse, no diag
// dependency — same discipline as whserver_health.cpp.

#ifndef TSDEV_WHSERVER_CENSUS_H
#define TSDEV_WHSERVER_CENSUS_H

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tsdev {

// Fixed WHServer VAs (32-bit, no ASLR — PROVEN, KB §20 / design §3.2).
// m_nLock counts client-held COM objects; m_bActivity is the monitor's
// self-reap flag (corroborating only, never a pass/fail input).
constexpr uintptr_t kWhLockVa     = 0x0043F8A8;
constexpr uintptr_t kWhActivityVa = 0x0043F8D8;

// Probed WHServer.exe build identity (SSOT). Every fixed VA above and
// every hardcoded vtable offset/dispid in whserver_recycle.cpp was
// measured on EXACTLY this build: 295,368 bytes, PE TimeDateStamp
// 2018-11-30 17:10:16 UTC, version resource 9.50.01.3344 "EasyLanguage
// Warehouse" (KB §17/§20). A TradeStation update that relayouts WHServer
// makes all of them wrong while the VAs may still map — the identity
// gate below must refuse BEFORE any fixed-offset read/call trusts them.
// Match = file size + PE TimeDateStamp + PE CheckSum of the on-disk
// image (the CheckSum is the linker's whole-image checksum).
constexpr ULONGLONG kWhServerImageSize = 295368;
constexpr DWORD     kWhServerTimeStamp = 0x5C016EF8;
constexpr DWORD     kWhServerCheckSum  = 0x00055BF2;

// File-level identity predicate: true ONLY when `imagePath` is the
// probed WHServer.exe build above (size + PE TimeDateStamp + PE CheckSum
// all match). Every failure direction fails closed — unreadable file,
// non-PE content, any field mismatch => false.
bool whServerImageIsKnown(const std::wstring& imagePath);

// PID-level wrapper: resolves the process image via
// OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) +
// QueryFullProcessImageNameW and applies whServerImageIsKnown. Any
// open/query failure => false.
bool whServerPidImageIsKnown(DWORD pid);

// A census target: which process to read and at which addresses. The VAs are
// parameterized so unit tests can read a known static in the test exe;
// production passes the fixed kWhLockVa/kWhActivityVa above.
struct CensusTarget {
    DWORD     pid;
    uintptr_t lockVa;
    uintptr_t activityVa;   // 0 = skip the corroborating activity read
};

// One per-pid read outcome (design §3.4 "lock value | openError | rpmError").
// openError is the OpenProcess GetLastError (0 = opened fine); rpmError is
// the ReadProcessMemory GetLastError for the m_nLock read (0 = read fine).
// lockValue is valid only when both errors are 0. The m_bActivity read is
// best-effort corroboration: a failed activity read sets activityValid=false
// but does NOT fail the sample (activity is never a pass/fail input).
struct WhLockSample {
    DWORD   openError;      // 0 = OpenProcess succeeded
    DWORD   rpmError;       // 0 = m_nLock RPM succeeded
    DWORD   lockValue;      // m_nLock, valid only when openError==rpmError==0
    uint8_t activity;       // m_bActivity (corroborating only)
    bool    activityValid;
};

// One poll of the census (level-1 input): a fresh re-enumerated pid set plus
// the per-pid reads attempted this poll. pids is meaningful only when
// snapshotOk; reads need not cover non-listed pids.
struct CensusPidRead {
    DWORD        pid;
    bool         attempted;   // false = listed pid with no read this poll
    // false = the pid's image failed the probed-build identity gate — the
    // sample is then untrusted (the fixed VAs are single-build layout
    // facts) => Indeterminate, same class as an openError on a listed pid.
    bool         imageKnown;
    WhLockSample sample;
};

struct CensusPoll {
    double                      timeSecs;    // monotonic poll timestamp
    bool                        snapshotOk;  // false = re-enumeration failed
    std::vector<DWORD>          pids;        // fresh pid set (snapshotOk only)
    std::vector<CensusPidRead>  reads;       // reads attempted this poll
};

enum class CensusVerdict { Pass, Refuse, Indeterminate };

// decideCensus result plus the offending pid/GetLastError that produced an
// Indeterminate (or the persisting pid on a lock>0 Refuse) so the engine can
// classify its refuse reason (design §3.3). Fields are 0 when not applicable.
struct CensusDecision {
    CensusVerdict verdict;
    DWORD         pid;    // offending/outstanding pid, 0 = none
    DWORD         error;  // GetLastError behind an Indeterminate, 0 = none
};

// Pure gate decision over a scripted/real sample stream. Polls must be in
// nondecreasing timeSecs order. windowSecs is the sustained-zero window
// (production >=3.0 s); budgetSecs is the census bound (production 30 s).
// Runs the §3.4 contract verbatim and returns at the first terminal
// condition: Indeterminate (immediate, never downgrades), Pass, or Refuse
// (budget elapsed / stream exhausted without a verdict — inconclusive
// streams fail closed).
CensusDecision decideCensus(const std::vector<CensusPoll>& polls,
                            double windowSecs,
                            double budgetSecs);

// Level-2 seam — the real read: OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION
// | PROCESS_VM_READ) + ReadProcessMemory at target.lockVa (and .activityVa
// when nonzero). Read-only; zero effect on the server. Returns true only
// when the lock DWORD was read (activity may still be invalid — check
// activityValid). On false, out.openError or out.rpmError carries the
// failing GetLastError.
bool readWhLockSample(const CensusTarget& target, WhLockSample& out);

} // namespace tsdev

#endif // TSDEV_WHSERVER_CENSUS_H
