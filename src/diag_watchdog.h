// diag_watchdog.h
//
// Watchdog thread for the §7.1 diagnostic build
// (reports/2026-09-02_v0.1.0_batch_hang_root_cause_analysis.md §7.1 2d/2e).
// Compiled ONLY when TSDEV_DIAG is defined; in the production build every
// macro expands to `((void)0)` and this TU is not linked.
//
// Behavior: if no site log record has been written for N seconds (default 60;
// env TSDEV_DIAG_WATCHDOG_SECS overrides; a value <= 0 DISABLES the watchdog
// entirely — no fire path — for long-idle roles like the `serve` broker,
// which idles by design while waiting for pipe clients), the watchdog
//   (1) appends ALL thread stacks of the process as module+offset to the log,
//   (2) appends the warehouse protocol state ([wh+0x04] cmd, [wh+0x6A] /
//       [wh+0x6E] / [wh+0x72] handle state, [wh+0x76] tid) if the wh pointer
//       was registered (obtained via the resolved GetWarehouse export); if it
//       is not obtainable it logs that explicitly and relies on stacks +
//       minidump,
//   (3) writes a minidump next to the log,
//   (4) aborts loudly: final log record, a clear stderr message naming the
//       watchdog and the log/dump paths, then terminates the process with
//       exit code 13 (unused elsewhere — see diag_watchdog.cpp).
// While idle it samples WHServer.exe liveness every 5 s (§7.1 2e).
//
// Idle-by-design phases (review F3b): code that enters a legitimately idle
// wait (the serve broker blocked in ConnectNamedPipe) brackets it with
// TSDEV_DIAG_IDLE_PHASE_BEGIN/END. While a phase is active the watchdog
// emits a periodic progress heartbeat record (every 15 s, or threshold/3 if
// smaller) instead of letting the idle time reach the fire threshold, so a
// healthy idle broker is never aborted (the 2026-09-02 smoke run was a
// healthy idle `serve` killed at 8.9 s idle). The markers are
// observation-only: they set/clear a flag and never block.

#ifndef TSDEV_DIAG_WATCHDOG_H
#define TSDEV_DIAG_WATCHDOG_H

#ifdef TSDEV_DIAG

// Start the file sink AND the watchdog thread (runs for the process
// lifetime). Called once at the top of main() in the diagnostic build.
#define TSDEV_DIAG_INIT() ::tsdev::diag::diagInit()

// Register the warehouse singleton pointer so the watchdog can read the
// protocol state. The pointer is obtained in tsdev_lib.cpp via the resolved
// GetWarehouse export (existing project knowledge, docs/KNOWLEDGE_BASE.md
// §2) — never by hacking orlib20 internals.
#define TSDEV_DIAG_SET_WAREHOUSE(ptr) \
    ::tsdev::diag::setWarehousePointer(ptr)

// Mark an idle-by-design phase (review F3b): while active, the watchdog
// emits a periodic progress heartbeat instead of counting the wait toward
// the fire threshold. `tag` must be a string literal valid for the phase
// duration. Observation-only; no-ops in the production build.
#define TSDEV_DIAG_IDLE_PHASE_BEGIN(tag) ::tsdev::diag::idlePhaseBegin(tag)
#define TSDEV_DIAG_IDLE_PHASE_END() ::tsdev::diag::idlePhaseEnd()

namespace tsdev {
namespace diag {

// initSink() + start the watchdog thread. If the sink cannot be created the
// watchdog is NOT started (a diag build must never abort a run because its
// own diagnostics failed).
void diagInit();

void startWatchdog();
void setWarehousePointer(void* wh);
void idlePhaseBegin(const char* tag);
void idlePhaseEnd();

}  // namespace diag
}  // namespace tsdev

#else  // !TSDEV_DIAG — production build: everything compiles out.

#define TSDEV_DIAG_INIT() ((void)0)
#define TSDEV_DIAG_SET_WAREHOUSE(ptr) ((void)0)
#define TSDEV_DIAG_IDLE_PHASE_BEGIN(tag) ((void)0)
#define TSDEV_DIAG_IDLE_PHASE_END() ((void)0)

#endif  // TSDEV_DIAG

#endif  // TSDEV_DIAG_WATCHDOG_H
