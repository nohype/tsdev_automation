// whserver_health.h
//
// WHServer.exe process discovery + user-address-space (VAS) headroom
// measurement.
//
// Motivation: a field crash showed WHServer.exe (32-bit, non-LARGEADDRESSAWARE)
// dying at "2047 MB user address space / 10 MB free" with std::bad_alloc in a
// blob-read COM handler. These helpers measure a target process's free user
// address space FROM OUR process via VirtualQueryEx (requires only
// PROCESS_QUERY_INFORMATION; we are elevated, same session, same bitness).
// kernel32 + toolhelp only — NO new .lib dependencies.
//
// The TU is pure Win32: no orlib20, no warehouse, no diag dependency.
// Currently shared: measureVasFree() is called by the diag watchdog's
// whserverSample() (whserver.sample records) and by the
// test_whserver_health unit test (which compiles this .cpp directly).
// whserverSample() deliberately keeps its own snapshotProcesses() +
// _wcsicmp pid-discovery loop instead of calling findWhServerPids():
// that path must log `process-snapshot-failed err=…` on snapshot failure,
// which findWhServerPids() cannot distinguish from "no WHServer running".
// findWhServerPids()' production consumers are the VAS-headroom guard
// (tsdev_cli.cpp) and the dispatch watchdog (dispatch_watchdog.cpp); it is
// also used by the unit test.

#ifndef TSDEV_WHSERVER_HEALTH_H
#define TSDEV_WHSERVER_HEALTH_H

#include <windows.h>

#include <cstdint>
#include <vector>

namespace tsdev {

// VAS headroom metrics for one target process. All fields are valid only when
// measureVasFree() returns true.
struct WhVasStats {
    uint64_t totalFreeBytes;         // sum of MEM_FREE region sizes below 2GB
    uint64_t largestFreeBlockBytes;  // largest single MEM_FREE region below 2GB
    uint64_t privateBytes;           // reserved for psapi GetProcessMemoryInfo;
                                     // always 0 (psapi is not linked — zero new
                                     // link deps is a hard requirement)
};

// All pids whose process image name is "WHServer.exe" (case-insensitive), via
// a single Toolhelp32 process snapshot. Empty when none are running (or the
// enumeration failed — discovery failure is indistinguishable and
// non-fatal for the caller's telemetry path).
std::vector<DWORD> findWhServerPids();

// snapshotOk variant of the same enumeration: returns false ONLY when the
// enumeration is inconclusive — the Toolhelp32 snapshot itself failed, or
// the process walk did not run to clean completion (a Process32FirstW or
// Process32NextW failure whose GetLastError is not ERROR_NO_MORE_FILES; a
// truncated set is never evidence). Callers that must distinguish
// "no WHServer running" from "enumeration inconclusive" — e.g. the census
// gate's per-poll re-enumeration, where a failed enumeration is never
// evidence of absence. On true, `pids` is the authoritative pid set
// (possibly empty). On false, `pids` is cleared.
bool findWhServerPids(std::vector<DWORD>& pids);

// Measure the user-address-space headroom of process `pid`: walks the target
// VAS with VirtualQueryEx from address 0 upward, summing MEM_FREE region sizes
// and tracking the largest MEM_FREE region. The walk stops at the 2 GB
// boundary (0x80000000): the crash target is a non-LARGEADDRESSAWARE 32-bit
// process whose usable user VAS ends there, and probing above would only
// touch kernel-reserved space. VirtualQueryEx returning 0 (end of VAS /
// region boundary beyond which the query is rejected) also ends the walk.
//
// Returns false when the process cannot be opened or the first query fails;
// `out` is only meaningful when true.
bool measureVasFree(DWORD pid, WhVasStats& out);

// Pure breach decision for the runBatch VAS-headroom guard, in MB so the
// caller's threshold config and the measured values share one unit.
// Breach semantics: STRICTLY BELOW — a measured value exactly equal to its
// threshold is healthy (pass). Breach when EITHER the total free VAS is
// below `thresholdTotalMB` OR the largest contiguous free block is below
// `thresholdContigMB` (fragmentation kills contiguous `new[]` allocations
// even while total free looks ample — the observed crash signature).
inline bool vasBreach(uint64_t totalFreeMB, uint64_t largestFreeMB,
                      uint64_t thresholdTotalMB, uint64_t thresholdContigMB) {
    return totalFreeMB < thresholdTotalMB ||
           largestFreeMB < thresholdContigMB;
}

// The guard's per-pid action under the two-threshold field policy (the
// 256/64 MB pair is the RESTART trigger, not a danger level -- the observed
// death zone is ~10 MB free; inserts cost ~0.07 MB each; a 435 MB library
// plateau sits below the trigger for the whole run):
//   Pass            no trigger breach -- healthy;
//   Continue        trigger breach still above the abort floor, but no
//                   recycle can happen (unauthorized, or a foreign-client
//                   latch says another TradeStation program holds the
//                   server) -- keep working;
//   AttemptRecycle  trigger breach with recycling authorized and not
//                   latched -- take the defer/engine recycle boundary;
//   Abort           below the abort floor with no authorization -- the
//                   hard-abort arm (batch exit 14 / serve exit-14 refusal).
enum class VasGuardAction {
    Pass,
    Continue,
    AttemptRecycle,
    Abort,
};

// Pure decision shared by the batch guard and the serve pre-mutation
// gate. Trigger semantics are vasBreach() against (triggerTotalMB,
// triggerContigMB); the abort floor is vasBreach() against (floorTotalMB,
// floorContigMB). foreignClients is the pre-activation latch (KB Section
// 21) -- consulted ONLY above the floor, where it downgrades an authorized
// recycle attempt to Continue; below the floor it is irrelevant (the
// defer/engine path aborts on any refuse, exactly as before).
inline VasGuardAction vasGuardAction(uint64_t totalFreeMB,
                                     uint64_t largestFreeMB,
                                     uint64_t triggerTotalMB,
                                     uint64_t triggerContigMB,
                                     uint64_t floorTotalMB,
                                     uint64_t floorContigMB,
                                     bool foreignClients,
                                     bool recycleAuthorized) {
    if (!vasBreach(totalFreeMB, largestFreeMB,
                   triggerTotalMB, triggerContigMB)) {
        return VasGuardAction::Pass;
    }
    if (vasBreach(totalFreeMB, largestFreeMB, floorTotalMB, floorContigMB)) {
        return recycleAuthorized ? VasGuardAction::AttemptRecycle
                                 : VasGuardAction::Abort;
    }
    // Trigger breach still above the abort floor -- survivable headroom.
    if (foreignClients || !recycleAuthorized) {
        return VasGuardAction::Continue;
    }
    return VasGuardAction::AttemptRecycle;
}

// Config sanity: the trigger must not sit below the abort floor. An
// inverted pair silently kills the floor in the band between them (the
// Pass check precedes the floor check), so reject the configuration at
// parse time instead of continuing through a dead safety net.
inline bool vasThresholdsInverted(uint64_t triggerTotalMB,
                                  uint64_t triggerContigMB,
                                  uint64_t floorTotalMB,
                                  uint64_t floorContigMB) {
    return triggerTotalMB < floorTotalMB ||
           triggerContigMB < floorContigMB;
}

} // namespace tsdev

#endif // TSDEV_WHSERVER_HEALTH_H
