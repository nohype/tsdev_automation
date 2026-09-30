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
// findWhServerPids() is used by the unit test today and is reserved for the
// upcoming production guard TVS.

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
// snapshot itself failed — discovery failure is indistinguishable and
// non-fatal for the caller's telemetry path).
std::vector<DWORD> findWhServerPids();

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

} // namespace tsdev

#endif // TSDEV_WHSERVER_HEALTH_H
