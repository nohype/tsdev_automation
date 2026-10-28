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
#include <string>
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

// Tri-state WHServer recycle authorization (the guard's decision input,
// resolved once per process from the flags/env -- see RecycleConfig in
// tsdev_cli.cpp):
//   Off        explicit opt-out (--no-recycle-whserver /
//              TSDEV_NO_RECYCLE_WHSERVER): the unauthorized behavior --
//              trigger-band breaches continue, a below-floor breach
//              hard-aborts (batch exit 14 / serve exit-14 refusal);
//   RescueOnly the DEFAULT: the recycle is reserved for the abort-floor
//              rescue -- a below-floor breach attempts the recycle
//              boundary, a trigger-band breach ALWAYS continues
//              in-process (the band is not a danger level; the observed
//              death zone is ~10 MB free);
//   Full       explicit opt-in (--recycle-whserver /
//              TSDEV_RECYCLE_WHSERVER): the full gated-recycle behavior
//              -- band breaches attempt the recycle too (when no
//              foreign-client latch holds the server).
// The zero-client census gate is orthogonal and applies in every mode
// that reaches the recycle boundary.
enum class RecycleMode {
    Off,
    RescueOnly,
    Full,
};

// The guard's per-pid action under the two-threshold field policy (the
// 256/64 MB pair is the RESTART trigger, not a danger level -- the observed
// death zone is ~10 MB free; inserts cost ~0.07 MB each; a 435 MB library
// plateau sits below the trigger for the whole run):
//   Pass            no trigger breach -- healthy;
//   Continue        trigger breach still above the abort floor, but no
//                   recycle can happen (mode is not Full, or a
//                   foreign-client latch says another TradeStation
//                   program holds the server) -- keep working;
//   AttemptRecycle  below the abort floor with mode != Off (the rescue),
//                   or a Full-mode band breach with no latch -- take the
//                   defer/engine recycle boundary;
//   Abort           below the abort floor with mode == Off -- the
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
// 21) -- consulted ONLY in the Full-mode band, where it downgrades a
// band recycle attempt to Continue; below the floor it is irrelevant
// (the defer/engine path aborts on any refuse, exactly as before).
// `mode` selects the tri-state policy: Off never attempts a recycle
// (below-floor -> Abort), RescueOnly attempts ONLY the below-floor
// rescue, Full attempts in the band too (latch permitting).
inline VasGuardAction vasGuardAction(uint64_t totalFreeMB,
                                     uint64_t largestFreeMB,
                                     uint64_t triggerTotalMB,
                                     uint64_t triggerContigMB,
                                     uint64_t floorTotalMB,
                                     uint64_t floorContigMB,
                                     bool foreignClients,
                                     RecycleMode mode) {
    if (!vasBreach(totalFreeMB, largestFreeMB,
                   triggerTotalMB, triggerContigMB)) {
        return VasGuardAction::Pass;
    }
    if (vasBreach(totalFreeMB, largestFreeMB, floorTotalMB, floorContigMB)) {
        // Below the abort floor: the rescue (RescueOnly and Full)
        // attempts the recycle boundary; Off hard-aborts.
        return mode != RecycleMode::Off ? VasGuardAction::AttemptRecycle
                                        : VasGuardAction::Abort;
    }
    // Trigger breach still above the abort floor -- survivable headroom.
    // Only an explicit Full authorization attempts a band recycle (and
    // never past the foreign-client latch); RescueOnly keeps working
    // in-process (its recycle is reserved for the floor rescue) and Off
    // always continues.
    if (mode == RecycleMode::Full && !foreignClients) {
        return VasGuardAction::AttemptRecycle;
    }
    return VasGuardAction::Continue;
}

// The machine-readable breach reason for the guard's abort/boundary
// summaries (the batch_completed abort_reason field and the serve
// refusal/recycle messages): name the metric that actually crossed the
// threshold pair passed here -- the pair that FIRED the abort. Callers
// pass the abort floor for the exit-14 hard-abort arms and for an
// authorized below-floor recycle boundary, and the restart trigger for
// an authorized above-floor boundary (vasGuardAction's own split), so
// the named metric is always the one whose threshold was crossed.
// When BOTH metrics crossed, the larger RELATIVE breach wins -- the
// smaller value/threshold ratio is the deeper breach; equal ratios
// tie-break to the largest-block arm (fragmentation is the sharper
// danger). Token format is pinned ("whserver_vas_free=<n>MB" /
// "whserver_vas_largest_free=<n>MB"); consumers key on the prefix and
// the exit code, never on which metric is named. Callers invoke this
// only after vasBreach() against the same pair returned true, so at
// least one metric crossed; the single-metric arms rely on that.
// Cross-multiplied integer comparison (all values positive), no FP:
// free's ratio is the smaller iff
// totalFreeMB*thresholdContigMB < largestFreeMB*thresholdTotalMB.
inline std::string vasAbortReason(uint64_t totalFreeMB,
                                  uint64_t largestFreeMB,
                                  uint64_t thresholdTotalMB,
                                  uint64_t thresholdContigMB) {
    const bool freeCrossed    = totalFreeMB  < thresholdTotalMB;
    const bool largestCrossed = largestFreeMB < thresholdContigMB;
    if (freeCrossed && largestCrossed) {
        if (totalFreeMB * thresholdContigMB <
            largestFreeMB * thresholdTotalMB) {
            return "whserver_vas_free=" + std::to_string(totalFreeMB) +
                   "MB";
        }
        // Larger relative breach, or the equal-ratio tie -> largest.
        return "whserver_vas_largest_free=" +
               std::to_string(largestFreeMB) + "MB";
    }
    if (freeCrossed) {
        return "whserver_vas_free=" + std::to_string(totalFreeMB) + "MB";
    }
    return "whserver_vas_largest_free=" + std::to_string(largestFreeMB) +
           "MB";
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
