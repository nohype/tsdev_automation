// diag_log.h
//
// Diagnostic file sink for the §7.1 diagnostic build
// (reports/2026-09-02_v0.1.0_batch_hang_root_cause_analysis.md §7.1).
//
// The entire module is compiled ONLY when TSDEV_DIAG is defined (the
// diagnostic build). In the production build every macro below expands to
// `((void)0)`, no diag symbol is referenced, and diag_log.cpp /
// diag_watchdog.cpp are not linked — the production tsdev_cli.exe stays
// behavior-identical with zero runtime cost.
//
// Sink contract (§7.1 2a):
//   - One log file per run: tsdev_diag_<pid>_<yyyymmdd_hhmmss>.log, created
//     in the process working directory (env var TSDEV_DIAG_DIR overrides the
//     directory), opened with FILE_FLAG_WRITE_THROUGH.
//   - Every record is a single WriteFile, written and flushed immediately, so
//     records survive process death (crash or hang-kill). A mutex serializes
//     writers so records stay atomic.
//   - Records carry a high-resolution QPC timestamp, the thread id, a site
//     tag, and a site-specific payload.

#ifndef TSDEV_DIAG_LOG_H
#define TSDEV_DIAG_LOG_H

#ifdef TSDEV_DIAG

// Write one record (immediately flushed). `site` is a string-literal tag;
// the rest is a printf-style payload that must produce a single line.
#define TSDEV_DIAG_LOG(site, ...) ::tsdev::diag::logRecordf(site, __VA_ARGS__)

namespace tsdev {
namespace diag {

// Create the log file (closes and replaces a previously initialized sink, so
// a test can re-initialize against a different directory). Returns false on
// failure (the sink then stays disabled and records become no-ops; the diag
// build must never crash the CLI because of diagnostics).
bool initSink();

// Full path of the current log file (UTF-8). Empty before a successful
// initSink().
const char* logPath();

// Write one record and flush it (single WriteFile under the sink mutex).
// Updates the watchdog progress timestamp (§7.1 2f).
void logRecordf(const char* site, const char* fmt, ...);

// Watchdog-facing variants (§7.1 2d/2e): write a record WITHOUT updating the
// watchdog progress timestamp — the watchdog's own records must not keep the
// watchdog fed, or it could never fire. lockSink()/unlockSink() let the
// watchdog hold the sink mutex across the suspend/stack-dump sequence so no
// other writer can be suspended while holding it (deadlock-free, and the
// whole dump stays atomic in the file).
void logRecordNoProgressf(const char* site, const char* fmt, ...);
void lockSink();
void unlockSink();

// Watchdog progress tracking (§7.1 2f): QPC of the last site record and the
// QPC frequency.
long long lastRecordQpc();
long long qpcFrequency();

}  // namespace diag
}  // namespace tsdev

#else  // !TSDEV_DIAG — production build: everything compiles out.

#define TSDEV_DIAG_LOG(site, ...) ((void)0)

#endif  // TSDEV_DIAG

#endif  // TSDEV_DIAG_LOG_H
