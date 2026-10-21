// tsdev_cli.cpp
//
// Production CLI for the TradeStation Development Environment library.
//
// Usage:
//   tsdev_cli [--json] [--ts-path <directory>] insert  <type> <name> <source_file>
//   tsdev_cli [--json] [--ts-path <directory>] modify  <type> <name> <source_file>
//   tsdev_cli [--json] [--ts-path <directory>] delete  <type> <name>
//   tsdev_cli [--json] [--ts-path <directory>] exists  <type> <name> [--case-insensitive]
//   tsdev_cli [--json] [--ts-path <directory>] get-source <type> <name> [--output <file>]
//   tsdev_cli [--json] [--ts-path <directory>] dependents <type> <name>
//   tsdev_cli [--json] [--ts-path <directory>] upsert <type> <name> <source_file>
//   tsdev_cli [--json] [--ts-path <directory>] batch-insert  <manifest_file>
//   tsdev_cli [--json] [--ts-path <directory>] batch-modify  <manifest_file>
//   tsdev_cli [--json] [--ts-path <directory>] batch-delete  <manifest_file>
//   tsdev_cli [--json] [--ts-path <directory>] batch-upsert  <manifest_file>
//   tsdev_cli [--json] [--ts-path <directory>] batch-insert-dir  <type> <dir_path> [-t <ext>...]
//   tsdev_cli [--json] [--ts-path <directory>] batch-modify-dir  <type> <dir_path> [-t <ext>...]
//   tsdev_cli [--json] [--ts-path <directory>] batch-delete-dir  <type> <dir_path> [-t <ext>...]
//   tsdev_cli [--json] [--ts-path <directory>] batch-upsert-dir  <type> <dir_path> [-t <ext>...]
//   tsdev_cli [--json] [--ts-path <directory>] serve [--serve-pipe-file <path>]
//   tsdev_cli [--json] [--ts-path <directory>] --backup-cache|--restore-cache [--cache-backup-policy=<mode>]
//                 (standalone cache maintenance — the flags also run before
//                 any subcommand, fixed order backup -> restore -> op)
//
//   <type>        "strategy", "function", "indicator", "tradingapp", or "showme" (case-insensitive)
//   <name>        component name
//   <source_file> path to a file containing EasyLanguage source (insert/modify)
//   <manifest_file> path to a line-based manifest (one item per line; '#'
//                 comments and blank lines are skipped). The op is fixed by the
//                 subcommand; each line carries only the per-item args:
//                   batch-insert/batch-modify/batch-upsert: <type> <name> <source_file>
//                   batch-delete:                              <type> <name>
//                 A <name> containing whitespace must be double-quoted
//                 ("my name"); a malformed quoted name is a
//                 pre-validation failure. <source_file> is the rest of
//                 the line, so it may contain spaces.
//   --json        Global flag. When present, emit a single JSON object on stdout
//                 (followed by a newline) describing the operation result, and
//                 suppress the human-readable stdout messages. Exit codes are
//                 unchanged in both modes. When absent, output is byte-identical
//                 to the pre-JSON behavior (backward compatible).
//                 Batch subcommands emit the JSON results array on stdout
//                 ONLY under --json; for batch-* (and tagged serve requests)
//                 --json also selects the stderr progress rendering — one
//                 compact JSON event object per line instead of the
//                 human-readable BATCH lines.
//   --pbar        Global, position-free boolean (bare "--pbar" or
//                 "--pbar=<value>"; the value is ignored — presence ==
//                 true — and repeats are harmless). Renders a pinned
//                 stderr-only progress bar that fills as items complete:
//                 0->N for batch-*, 0->1 for the single-item subcommands.
//                 Quiet on success: successful items and a fully clean
//                 batch emit nothing but the bar itself; failures,
//                 warnings, and a non-clean BATCH COMPLETE scroll above it.
//                 stderr only — stdout (results / JSON) is never touched.
//                 Cannot be combined with --json; not valid with serve.
//   --backup-cache / --restore-cache
//                 Global, position-free presence booleans. When either is
//                 given the cache phase runs once, after all validation and
//                 the elevation guard but BEFORE any subcommand dispatch
//                 (backup -> restore -> subcommand, regardless of argv
//                 order); any cache-phase failure aborts the run before the
//                 subcommand. With no subcommand they run standalone and
//                 print a stdout result. Not valid with serve.
//   --cache-backup-policy=<prompt|overwrite|skip|abort>
//                 What --backup-cache does when a backup already exists.
//                 Requires --backup-cache (a usage error without it).
//                 Default prompt: interactive consoles get an
//                 O(verwrite)/S(kip)/C(ancel) prompt read from CONIN$
//                 (never stdin — --batch-stdin owns it); non-interactive
//                 prompt behaves as skip (keep existing backup, continue).
//                 abort cancels the run with exit 17 before any operation.
//                 The cache dir is <root>\Program\Cache resolved via
//                 resolveTsInstallRoot — a --ts-path sandbox only needs
//                 <root>\Program\Cache to exist for the cache phase; a stub
//                 orlib20.dll is required only when an OP also runs
//                 against the sandbox.
//   --no-cache-compress
//                 Global, position-free presence boolean. Suppresses the
//                 default post-backup kick: after a successful backup phase
//                 (fresh or kept-existing) a detached lowest-priority
//                 background thread NTFS-compresses the backup's cache\ tree
//                 in place (FSCTL_SET_COMPRESSION, files >= 64 KB only).
//                 Transparent to restore; a partially compressed tree is
//                 always valid. Backup tree only — restore output and the
//                 live cache are never compressed.
//   -v/--verbose  Global, position-free, repeatable flag. Each occurrence
//                 raises the runtime log level (tsdev_lib) one step:
//                 default = warnings + errors only; 1 occurrence (-v) = + info;
//                 2+ occurrences (-vv) = + debug internals (library traces).
//                 Additional occurrences clamp at debug. Accepted forms:
//                 -v, -vv, -vvv (bundled), --verbose, --verbose=<value>
//                 (value ignored, counts as ONE occurrence). "Space + value"
//                 (--verbose 2) is NOT supported. All log output goes to
//                 stderr only; stdout (results and JSON) is unaffected.
//   --ts-path <directory>  Override the TradeStation install directory
//                          (default: auto-discovered from registry)
//
// The CLI wraps the shared tsdev_lib module (which wraps orlib20.dll). It:
//   1. Reads the source file
//   2. Constructs TsDevLib (loads orlib20, patches, inits)
//   3. Checks if the component already exists (fail loudly — no overwrite)
//   4. Creates the entry, saves source, compiles/verifies
//   5. Fails loudly on compilation errors (prints to stderr, exit non-zero)
//   6. Closes the warehouse and verifies the .ell file exists
//   7. Prints success with ProgId
//
// Exit codes: 0=success, 1=usage/arg error, 2=unknown type, 3=source file
// error, 4=init failure, 5=not-found (modify/delete) OR already-exists
// (insert) — also: no cache backup found on restore-cache — 6=entry op
// failure (insert/modify/delete), 7=saveSource failure,
// 8=compile error, 9=.ell missing,
// 10=CreateThread failure, 11=not elevated.
// 12 is used by the Python client (ElevationRequiredError,
// src/tsdev_automation/exceptions.py). 13 = diagnostic-build watchdog abort
// (tsdev_cli_diag only; see src/diag_watchdog.cpp). 14 = batch aggregate:
// aborted by the WHServer VAS-headroom guard (see batch exit-code scheme
// below). 15 = dispatch watchdog abort (TVS-3; see src/dispatch_watchdog.cpp).
// 16 = cache I/O error (backup/restore failure, cache dir or backup root
// unreachable, restore of a locked cache); 17 = cache backup cancelled
// (--cache-backup-policy=abort or 'C' at the prompt).
// 18 = WHServer recycled on a VAS-guard breach under --recycle-whserver;
// 19 = WHServer recycle refused.
// 20-24 = own-process fatal-termination containment (P4 gap-e; the silent-
// death block above vehCrashHandler): 20 = std::terminate (unhandled C++
// exception / noexcept violation), 21 = abort()/SIGABRT, 22 = CRT
// invalid-parameter fatal, 23 = pure virtual call, 24 = UEF invoked with
// no exception record (defensive — unreachable per the API contract).
// VEH/UEF deaths otherwise exit with the SEH exception code itself
// (0xC...), never via WER/TSCrashReport and never through ExitProcess's
// DLL detach.
// Every direct tsdev_cli subcommand, including serve, requires an elevated
// administrator process; when the guard runs non-elevated it returns exit 11.
//
// Batch subcommands (batch-insert/batch-modify/batch-delete) use a separate
// exit-code scheme (they are distinct subcommands, so no conflict):
//   0 = all items succeeded
//   1 = usage error / manifest file not found / manifest empty (batch did not run)
//   2 = batch ran but one or more items failed (including an explicit close
//       failure; per-item details are in the JSON array under --json, else
//       on stderr)
//   3 = batch cooperatively cancelled via --batch-progress-control (the
//       stdout array stays full length; unattempted items are exit-12
//       "cancelled" markers)
//  14 = batch aborted by the WHServer VAS-headroom guard (same full-length
//       array + exit-12 not-run markers for unattempted items)
//
// WHServer VAS-headroom guard (batch-* and serve): WHServer.exe is 32-bit
// and non-LARGEADDRESSAWARE; a field crash showed it dying at ~10 MB free
// user address space (std::bad_alloc inside a COM handler). After each
// completed non-final item the batch measures WHServer's free VAS (see
// whserver_health.h); when the total free MB drops below --whserver-free-mb
// (default 256) or the largest contiguous free block drops below
// --whserver-largest-free-mb (default 64) the batch aborts LOUDLY before
// dispatching the next item: remaining items become exit-12 not-run markers
// and the process exits 14. On the serve path the same gate runs before
// dispatching each mutating request (insert/modify/delete/upsert): a breach
// returns an exit-14 refusal response (error.code "whserver_vas_abort")
// without executing the op — the client treats it as a batch abort.
// WHServer not running (lazy singleton not yet spawned) = pass; a transient
// measurement failure warns once and does NOT abort/refuse.
// --no-whserver-vas-guard disables the guard. Env mirrors:
// TSDEV_WHSERVER_FREE_MB / TSDEV_WHSERVER_LARGEST_FREE_MB /
// TSDEV_NO_WHSERVER_VAS_GUARD (explicit CLI flags win over env). NOTE:
// ShellExecuteExW runas does NOT inherit the caller's environment block, so
// the Python client forwards TSDEV_WHSERVER_* overrides to the serve broker
// as explicit --whserver-* flags (see _build_serve_parameters).
//
// --recycle-whserver (batch-*/serve only) authorizes restarting WHServer.exe
// when the VAS-headroom guard trips (design: tmp/RECYCLE_ENGINE_DESIGN.md).
// The flag AUTHORIZES the recycle — it never bypasses the safety gate: the
// engine still requires a zero-client census, and a gate/stage refusal exits
// 19 (whserver_recycle_refused); a successful recycle exits 18
// (whserver_recycled) so the batch resumes in a fresh process (serve: the
// elevated broker respawns). R1/R2 site rule (PROVEN live findings): the
// engine only runs in-process at NEVER-ATTACHED sites — an attached
// process holds +7 m_nLock for its whole lifetime (closeWarehouse cannot
// release it, and ONLY graceful teardown can — a TerminateProcess corpse
// orphans it for >=minutes, deadlocking the resumed census; live-proven
// 2026-10-21), so an attached-site breach exits 18 WITHOUT running the
// engine (reason=recycle_deferred_own_session) — but FIRST runs the §5
// bounded close stage so the process exits GRACEFULLY and releases the
// +7; a failed/uncertain close refuses on the close-stage family (exit
// 19, TerminateProcess-self). The resumed process/broker runs the engine
// at its own never-attached guard site. Env mirror: TSDEV_RECYCLE_WHSERVER (any value
// but "0"; the explicit flag wins over env "0" — OR-ed presence semantics).
// Conflicts (usage error exit 1): --no-whserver-vas-guard (a recycle can
// never trigger with the guard disabled) and --batch-progress-control (the
// stdin control channel belongs to the dying process — a mid-batch recycle
// exit would strand it). The §6.3 residual-race warning is printed once per
// process lifetime when the flag is in effect. The recycle boundary is
// wired at all three guard sites and exits 18/19 are emitted with the
// recycle not-run markers appended to the results array (the error.code
// mappings are live). On a recycle exit the CLI also writes a resume manifest
// (tsdev_resume_<pid>_<ticks>.manifest beside the source manifest, or the
// cwd for --batch-stdin / the scanned directory for batch-*-dir) covering
// the not-run tail; resubmitting `batch-<op> <resume-file>` re-attempts
// exactly that tail. In serve mode the Python client respawns the broker
// and resubmits from the refused index; on the direct path it resubmits
// the resume manifest automatically (both bounded: 3 recycles per batch
// call); exit 19 aborts.
//
// Batch progress is always emitted on stderr (independent of -v): one
// BATCH SUCCESS/BATCH FAILED completion line per attempted item (a FAILED
// line is followed by the item's full human detail block when present),
// then a single BATCH COMPLETE line after the end-of-batch warehouse close
// (+ result rewrite). With --json each event is one compact JSON object per
// line instead, and a batch_item_started event additionally precedes each
// attempted item's completion (the client pairs them).
//
// --batch-progress-control (batch-* subcommands only) turns stdin into a
// cooperative control channel: after each completed non-final item the
// batch reads exactly one decision line — "continue\n" or "cancel\n" —
// before attempting the next item. With --batch-stdin the manifest itself
// is first read as a framed block
//   TSDEV_BATCH_CONTROL/1 <decimal byte length>\n<exactly length bytes>
// and decision lines follow on the same stream; with a manifest path or a
// batch-*-dir subcommand stdin carries decision lines only. "cancel", EOF,
// or a malformed token stops the batch before the next item: the remaining
// items are reported as exit-12 not-run markers in the still-full-length
// stdout array and the process exits 3. A malformed control input emits a
// batch_control_error / BATCH CONTROL ERROR diagnostic on stderr before
// the cancellation.
//
// NOTE: The EasyLanguage compiler (invoked via VerifyModule_New) requires a
// large stack, AND the hidden EDIT control created by TsDevLib is a Win32
// window (thread-affine). The reference probe (archive/elevated_test20.cpp) runs its
// ENTIRE flow in a worker thread with a 64 MB stack. We do the same: all
// TsDevLib operations run on a 64 MB-stack worker thread.

#include "tsdev_lib.h"
#include "persistence_integrity.h"  // TVS-L.9: shared post-verify validator
#include "create_path_cleanup.h"    // TVS-L.10: create-path cleanup ownership
#include "diag_log.h"        // §7.1 diagnostic build (no-op without TSDEV_DIAG)
#include "diag_watchdog.h"   // §7.1 diagnostic build (no-op without TSDEV_DIAG)
#include "dispatch_watchdog.h"  // TVS-3: production bounded-wait dispatch containment
#include "whserver_health.h" // TVS-1b: WHServer VAS-headroom guard (batch)
#include "whserver_recycle.h" // R3: gated WHServer recycle engine
#include "ts_install.h"     // resolveTsCacheDir for the cache phase
#include "cache_backup.h"   // cache backup/restore engine (--backup-cache/--restore-cache)

#include <nlohmann/json.hpp>  // TVS-D.2: --json structured output

#include <windows.h>
#include <bcrypt.h>           // TVS-L.9: SHA-256 of pre-op .ell/.xml artifacts

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <cctype>
#include <vector>
#include <utility>
#include <filesystem>
#include <io.h>
#include <fcntl.h>
#include <atomic>      // TVS-K.4: std::atomic<bool> for serve quit flag
#include <chrono>      // cache-phase progress renderer timing/throttle
#include <csignal>     // signal/SIGABRT — P4 gap-e fatal containment
#include <exception>   // std::set_terminate — P4 gap-e fatal containment
#include <cwctype>     // towlower for the cache-backup prompt key read
#include <new>         // std::nothrow (bounded control-read job alloc, R6)
#include <process.h>   // _beginthreadex (bounded control-read helper, R6)
#include <sddl.h>      // TVS-K.4: ConvertStringSecurityDescriptorToSecurityDescriptorW

using namespace tsdev;
using ordered_json = nlohmann::ordered_json;  // preserves insertion order

namespace {

// P4 gap-e own-process fatal-termination containment. The definitions —
// with the full rationale and the orlib20 UEF-ordering analysis — live next
// to vehCrashHandler below; the worker procs precede them in this file, so
// the two install entry points are forward-declared here.
void installThreadFatalContainment();
void installProcessUef();

// --- Helpers ---

// Case-insensitive ASCII string comparison.
bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(),
        [](char ca, char cb) {
            return std::tolower(static_cast<unsigned char>(ca)) ==
                   std::tolower(static_cast<unsigned char>(cb));
        });
}

// Parse the <type> argument into a ComponentType. Returns false on unknown.
bool parseType(const std::string& s, ComponentType& out) {
    if (iequals(s, "strategy")) { out = ComponentType::Strategy; return true; }
    if (iequals(s, "function")) { out = ComponentType::Function;  return true; }
    if (iequals(s, "indicator")) { out = ComponentType::Indicator; return true; }
    if (iequals(s, "tradingapp")) { out = ComponentType::TradingApp; return true; }
    if (iequals(s, "showme")) { out = ComponentType::ShowMe; return true; }
    return false;
}

// Read an entire file into a string. Returns false on failure.
bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Write a string to a file (binary mode). Returns false on failure.
bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return f.good();
}

// Elevation guard (TVS-K.1): query whether the current process is running
// with an elevated (administrator) token. Uses the Windows token-API
// sequence: OpenProcessToken -> GetTokenInformation(TokenElevation). On any
// API failure, returns false (fail safe: treat as not elevated).
bool isElevated() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return false;  // fail safe: treat as not elevated
    TOKEN_ELEVATION elev = {};
    DWORD ret = 0;
    bool ok = GetTokenInformation(hToken, TokenElevation, &elev,
                                  sizeof(elev), &ret);
    CloseHandle(hToken);
    return ok && elev.TokenIsElevated != 0;
}

// TVS (CLI log levels): map the accumulated -v/--verbose occurrence count to
// the runtime library log level. 0 occurrences -> Warning (default:
// warnings + errors only), 1 -> Info, >=2 -> Debug (clamped; further
// occurrences have no additional effect). Pure helper, file-local so the
// same-TU unit test (tests/test_log_levels.cpp) can exercise the mapping
// incl. clamping.
TsDevLogLevel LogLevelFromVerbosityCount(int count) {
    if (count <= 0) return TsDevLogLevel::Warning;
    if (count == 1) return TsDevLogLevel::Info;
    return TsDevLogLevel::Debug;
}

void printUsage() {
    std::fprintf(stderr,
        "Usage:\n"
        "  tsdev_cli [--json] [--ts-path <directory>] insert  <type> <name> "
        "<source_file>\n"
        "  tsdev_cli [--json] [--ts-path <directory>] modify  <type> <name> "
        "<source_file>\n"
        "  tsdev_cli [--json] [--ts-path <directory>] delete  <type> <name>\n"
        "  tsdev_cli [--json] [--ts-path <directory>] exists  <type> <name> "
        "[--case-insensitive]\n"
        "  tsdev_cli [--json] [--ts-path <directory>] get-source <type> <name> "
        "[--output <file>]\n"
        "  tsdev_cli [--json] [--ts-path <directory>] dependents <type> <name>\n"
        "  tsdev_cli [--json] [--ts-path <directory>] upsert <type> <name> "
        "<source_file>\n"
        "  tsdev_cli [--json] [--ts-path <directory>] batch-insert  <manifest_file> | --batch-stdin\n"
        "  tsdev_cli [--json] [--ts-path <directory>] batch-modify  <manifest_file> | --batch-stdin\n"
        "  tsdev_cli [--json] [--ts-path <directory>] batch-delete  <manifest_file> | --batch-stdin\n"
        "  tsdev_cli [--json] [--ts-path <directory>] batch-upsert  <manifest_file> | --batch-stdin\n"
        "  tsdev_cli [--json] [--ts-path <directory>] batch-insert-dir  <type> <dir_path> [-t <ext>...]\n"
        "  tsdev_cli [--json] [--ts-path <directory>] batch-modify-dir  <type> <dir_path> [-t <ext>...]\n"
        "  tsdev_cli [--json] [--ts-path <directory>] batch-delete-dir  <type> <dir_path> [-t <ext>...]\n"
        "  tsdev_cli [--json] [--ts-path <directory>] batch-upsert-dir  <type> <dir_path> [-t <ext>...]\n"
        "  tsdev_cli [--json] [--ts-path <directory>] serve [--serve-pipe-file <path>]\n"
        "  <type>         strategy | function | indicator | tradingapp | showme (case-insensitive)\n"
        "  <name>         component name\n"
        "  <source_file>  path to a file containing EasyLanguage source "
        "(insert/modify)\n"
        "  <manifest_file> path to a line-based manifest (one item per line;\n"
        "                  '#' comments and blank lines are skipped). Lines:\n"
        "                  insert/modify/upsert: <type> <name> <source_file>\n"
        "                  delete:               <type> <name>\n"
        "                  A <name> containing whitespace must be double-quoted\n"
        "                  (\"my name\"); <source_file> is the rest of the line\n"
        "                  and may contain spaces.\n"
        "  --batch-stdin    read the manifest from stdin instead of <manifest_file>\n"
        "                   (same line format; stdin is read in binary mode).\n"
        "  --batch-progress-control  (batch-* only) cooperative between-items\n"
        "                   control on stdin: after each completed non-final item\n"
        "                   the batch waits for exactly \"continue\\n\" or "
        "\"cancel\\n\".\n"
        "                   With --batch-stdin the manifest is first read as a\n"
        "                   framed TSDEV_BATCH_CONTROL/1 <byte length>\\n + "
        "payload\n"
        "                   block; with a manifest path or batch-*-dir, stdin\n"
        "                   carries decision lines only. cancel/EOF/malformed\n"
        "                   input stops before the next item: remaining items\n"
        "                   become exit-12 not-run markers (full-length array)\n"
        "                   and the process exits 3.\n"
        "  --batch-control-timeout-secs <N>  (batch-* with\n"
        "                   --batch-progress-control) bound each decision wait:\n"
        "                   a stalled/open control channel (e.g. a dead parent)\n"
        "                   cancels the batch after N seconds instead of\n"
        "                   hanging forever. Default 900; 0 disables the bound;\n"
        "                   valid range 0..86400.\n"
        "  --whserver-free-mb <N>  (batch-*/serve) WHServer VAS-headroom guard:\n"
        "                   abort the batch (exit 14, remaining items become\n"
        "                   exit-12 not-run markers) before the next item when\n"
        "                   WHServer.exe total free user address space drops\n"
        "                   below N MB. Default 256. Env: TSDEV_WHSERVER_FREE_MB.\n"
        "  --whserver-largest-free-mb <N>  (batch-*/serve) same guard on the\n"
        "                   largest contiguous free block (fragmentation kills\n"
        "                   contiguous allocations). Default 64.\n"
        "                   Env: TSDEV_WHSERVER_LARGEST_FREE_MB.\n"
        "  --no-whserver-vas-guard  (batch-*/serve) disable the WHServer\n"
        "                   VAS-headroom guard entirely. Env:\n"
        "                   TSDEV_NO_WHSERVER_VAS_GUARD (any value but \"0\").\n"
        "  --recycle-whserver  (batch-*/serve) authorize restarting\n"
        "                   WHServer.exe when the VAS-headroom guard trips.\n"
        "                   The restart is still gated on a zero-client\n"
        "                   census — the flag AUTHORIZES the recycle, it does\n"
        "                   NOT bypass the census gate: when the gate cannot\n"
        "                   confirm zero foreign-held objects the batch aborts\n"
        "                   (exit 19). A successful recycle exits 18 so the\n"
        "                   batch resumes in a fresh process (serve: the\n"
        "                   elevated broker respawns — a non-elevated client\n"
        "                   gets one UAC prompt per respawn). Not valid with\n"
        "                   --no-whserver-vas-guard or --batch-progress-control.\n"
        "                   Presence-only: any '=' value is ignored; no\n"
        "                   negative form — env \"0\" disables the env arm\n"
        "                   only, never the flag.\n"
        "                   Env: TSDEV_RECYCLE_WHSERVER (any value but \"0\").\n"
        "                   (On a recycle exit the CLI writes a resume\n"
        "                   manifest covering the not-run tail — resubmit\n"
        "                   `batch-<op> <resume-file>`; the Python client\n"
        "                   resubmits it automatically, bounded at 3 recycles\n"
        "                   per batch — exit 19 aborts.)\n"
        "  --traverse-strategy-dependents  (all verify paths: single ops,\n"
        "                   batch-*, serve) restore the legacy per-item\n"
        "                   whole-library reverse-dependent traversal for\n"
        "                   strategy roots (skipped by default: strategies\n"
        "                   cannot be dependency targets). Env:\n"
        "                   TSDEV_TRAVERSE_STRATEGY_DEPENDENTS (any value but \"0\").\n"
        "  --dispatch-timeout-secs <N>  (all subcommands) bounded-wait\n"
        "                   containment: abort the process with exit 15 when a\n"
        "                   warehouse dispatch stays in-flight longer than N\n"
        "                   seconds (WHServer wedged/crashed -> orlib20's\n"
        "                   synchronous COM call never returns). Default 300;\n"
        "                   0 disables the watchdog; valid range 0..86400.\n"
        "                   Env: TSDEV_DISPATCH_TIMEOUT_SECS (flag wins).\n"
        "  --json         Emit a single JSON result object on stdout (suppress "
        "human-readable stdout).\n"
        "                 For batch-* (and tagged serve requests) it also\n"
        "                 selects the stderr progress rendering: one JSON\n"
        "                 event object per line instead of human-readable\n"
        "                 BATCH lines. batch-* emit the JSON results array\n"
        "                 on stdout only under --json.\n"
        "  --pbar         Pinned progress bar on stderr, filling as items\n"
        "                 complete (0->N batch-*, 0->1 single ops). Global,\n"
        "                 position-free; '--pbar=<v>' accepted (value ignored).\n"
        "                 Silent while items succeed; failures and warnings\n"
        "                 scroll above the bar, and BATCH COMPLETE prints\n"
        "                 only when the batch was not fully clean.\n"
        "                 Not with --json; not valid with serve.\n"
        "  --backup-cache  Back up <TS>\\Program\\Cache to\n"
        "                 %LOCALAPPDATA%\\tsdev_cli\\cache-backup before the\n"
        "                 subcommand (fixed order: backup -> restore -> op).\n"
        "                 Standalone: tsdev_cli --backup-cache\n"
        "                 [--cache-backup-policy=...].\n"
        "  --restore-cache  Restore the backup into the Cache dir first\n"
        "                 (exit 5 when no backup exists; exit 16 when the cache\n"
        "                 is locked by a running TradeStation).\n"
        "  --cache-backup-policy <prompt|overwrite|skip|abort>  What to do when\n"
        "                 a backup already exists (default prompt; a\n"
        "                 non-interactive prompt behaves as skip; abort cancels\n"
        "                 with exit 17). Requires --backup-cache. The cache\n"
        "                 flags are not valid with serve.\n"
        "  --no-cache-compress  Suppress the background NTFS in-place\n"
        "                 compression of the backup tree that otherwise\n"
        "                 starts after a successful backup phase (fresh or\n"
        "                 kept-existing). Compression is transparent to\n"
        "                 restore; only the backup dir is touched, never the\n"
        "                 live cache.\n"
        "                 Output placement: standalone (no subcommand) prints\n"
        "                 BACKUP COMPLETE / BACKUP KEPT / RESTORE COMPLETE — or\n"
        "                 the --json object — on stdout; combined with a\n"
        "                 subcommand the cache phase emits stderr lines only.\n"
        "                 Standalone --json object: \"op\" plus a \"cache\"\n"
        "                 payload for a single flag, or \"backup\"+\"restore\"\n"
        "                 payloads for the two-flag combo; a skipped backup\n"
        "                 adds \"skipped\":true,\"created\":...\n"
        "  --ts-path <directory>  Override the TradeStation install directory "
        "(default: auto-discovered from registry)\n"
        "  -v, --verbose  Raise the stderr log level. Position-free global "
        "flag, repeatable:\n"
        "                 default = warnings + errors only; one occurrence "
        "(-v) = + info;\n"
        "                 two or more (-vv) = + debug internals (library "
        "traces). Additional\n"
        "                 occurrences clamp at debug. Accepted forms: -v, "
        "-vv, -vvv\n"
        "                 (bundled), --verbose, --verbose=<value> (value "
        "ignored, counts as\n"
        "                 one occurrence). Log output goes to stderr only; "
        "stdout (results\n"
        "                 and JSON) is unaffected.\n"
        "  --output <file>  (get-source only) Write the retrieved EasyLanguage "
        "source to <file>\n"
        "                   instead of stdout. Internally encrypted items are "
        "decrypted automatically.\n"
        "  --case-insensitive  (exists only) Match the name case-insensitively.\n"
        "                      Without this flag, the name must match exactly.\n"
        "  -t <ext>       (batch-*-dir only) Filter files by extension. Repeatable.\n"
        "                 First -t overrides defaults (.el, .txt); subsequent -t flags add.\n"
        "                 Extensions are case-insensitive; dot prefix optional (el == .el).\n"
        "                 File stem (sans last extension) = item name.\n"
        "  --serve-pipe-file <path>  (serve only) Write the named pipe path and PID to\n"
        "                 <path> once the pipe is created (atomic: .tmp + rename).\n"
        "                 If absent, the pipe name is printed to stdout instead.\n"
        "  batch-* emit a JSON array on stdout (one element per item) only\n"
        "                 under --json, and always emit per-item progress on\n"
        "                 stderr: a BATCH SUCCESS/FAILED completion line per\n"
        "                 attempted item (failures followed by the item's full\n"
        "                 detail) plus a final BATCH COMPLETE line. --json\n"
        "                 renders one JSON event object per line and adds a\n"
        "                 batch_item_started event before each completion;\n"
        "                 --pbar replaces the lines with a pinned bar.\n");
}

// --- TVS-D.2: structured result + output -------------------------------------

// CliResult captures everything needed to render the result in EITHER the
// human-readable form (existing, byte-identical) or the JSON form (new).
// Each run*/cmd* path populates exactly one CliResult; a single emitOutput()
// call then renders it. This avoids duplicating the operation logic across
// the two output modes and keeps the diff focused.
//
// Fields:
//   op            "insert"/"modify"/"delete" (empty when unknown, e.g. usage
//                 errors before the subcommand is fully resolved).
//   typeStr       the <type> argument verbatim (empty when unknown).
//   name          the component name (empty when unknown).
//   success       true iff the operation succeeded (exit 0).
//   exitCode      the exit code (unchanged in both modes).
//   progId        the ProgId, when known (success, and some error cases).
//   ellPath       the .ell file path (success insert/modify only; absent for
//                 delete success).
//   existingProgId  the ProgId of an already-existing entry (insert exit 5).
//   compileErrors the raw compiler error text (exit 8 only).
//   humanStdout   the EXACT human-readable text to write to stdout (success
//                 cases). Empty for error cases.
//   humanStderr   the EXACT human-readable text to write to stderr (error
//                 cases). Empty for success cases.
//
// The JSON error.message field is derived from a concise, machine-friendly
// message (errorMessage), distinct from the prefixed humanStderr line.
struct CliResult {
    std::string op;
    std::string typeStr;
    std::string name;
    bool        success = false;
    int         exitCode = 0;
    int         progId = 0;
    std::string ellPath;
    int         existingProgId = 0;
    std::string compileErrors;
    std::string humanStdout;
    std::string humanStderr;
    std::string errorMessage;  // concise message for JSON error.message
    std::string cleanupError;  // TVS-L.10: secondary create-path cleanup error
                                // (non-empty only when partial-entry cleanup was
                                // attempted after a post-createEntry failure and
                                // the cleanup itself failed). Preserves the
                                // primary diagnostic; cleanup failure never
                                // flips a failure into a success.
    bool        exists = false;  // "exists" op: whether the item was found
    std::string storedName;  // "exists" op: verbatim stored name (empty if not found)
    std::string sourceText;  // "get-source" op: retrieved EL source (JSON mode)
    // M.4b.1: "dependents" op — reverse dependents (callers) from the
    // warehouse-level cmd 0x0D → cmd 0x3B path. Empty when the op is not
    // "dependents" or when no callers were found.
    std::vector<TsDevLib::CallerInfo> callers;
    // Serve cache ops (backup-cache/restore-cache): extra top-level members
    // merged into the success JSON response — carries the exact standalone
    // --json cache payload (the "cache" member object). Null/empty for all
    // other ops (no serialization change).
    ordered_json extraPayload;
    // §5 teardown flag (whserver_recycle.h close_failed class): true when
    // the op ended while the warehouse session may still be open with
    // UNCERTAIN teardown state — i.e. a closeWarehouse that this op owned
    // did not provably complete (doInsert step 8, or a deleteEntry failure
    // classified by deleteResultLeavesSessionOpen). The session-owning
    // worker must then emit pending output + TerminateProcess instead of
    // `delete lib`/normal exit (singleOpTerminateIfSessionOpen). Never set
    // for an intact session: a healthy open session + OrlibExit is the
    // proven normal path (modify/exists/get-source never close at all).
    // INTERNAL ONLY — never serialized by cliResultToJson.
    bool        sessionMayBeOpen = false;
};

// Map an exit code to the machine-readable error.code string (TVS-D.2 §3).
// The `op` disambiguates exit 5 (insert -> already_exists, modify/delete ->
// not_found) and exit 3 (get-source -> output_write_error, else
// source_file_error).
std::string errorCodeString(int exitCode, const std::string& op) {
    switch (exitCode) {
        case 1:  return "usage_error";
        case 2:  return "unknown_type";
        case 3:  return (op == "get-source") ? "output_write_error" : "source_file_error";
        case 4:  return "init_failure";
        case 5:  return (op == "insert") ? "already_exists" : "not_found";
        case 6:  return "entry_op_failure";
        case 7:  return "save_source_failure";
        case 8:  return "compile_error";
        case 9:  return "artifact_missing";
        case 10: return "thread_failure";
        case 11: return "not_elevated";
        case 12: return "cancelled";
        // 14 is the WHServer VAS-guard abort code: the batch-aggregate exit
        // (batch per-item markers stay exit 12) and the serve-path per-item
        // refusal code.
        case 14: return "whserver_vas_abort";
        // 15 is the dispatch-watchdog abort code (TerminateProcess(15) from
        // the watchdog thread in src/dispatch_watchdog.cpp). A watchdog
        // abort emits no JSON frame — the "WATCHDOG ABORT: ...; exiting
        // with code 15" line goes to stderr — so this case is unreachable
        // through normal result emission; kept for completeness so any
        // future path that does surface exit 15 gets a stable code string.
        case 15: return "whserver_dispatch_watchdog_abort";
        // 16/17 are the cache-phase codes (--backup-cache/--restore-cache):
        // 16 any cache I/O failure (also covers restore of a locked cache),
        // 17 a cancelled backup (policy abort or 'C' at the prompt).
        case 16: return "cache_io_error";
        case 17: return "cache_cancelled";
        // 18/19 are the recycle boundary codes (design §6.2): 18 = the
        // engine recycled WHServer (the refused item was never
        // dispatched — resume in a fresh process / respawned broker);
        // 19 = the gate or a fail-closed stage refused the recycle and
        // the batch aborted.
        case 18: return "whserver_recycled";
        case 19: return "whserver_recycle_refused";
        default: return "unknown_error";
    }
}

// cliResultToJson serializes a CliResult to an ordered_json object using the
// EXACT schema shared by emitOutput()'s JSON mode and emitBatchOutput()'s
// per-result array entries (TVS-K.2). Extracted from emitOutput() so the
// serve layer (and both emit paths) share a single serialization function.
//
// op/type/name are included only when known (omitted for usage errors that
// occur before the subcommand/args are resolved). On success, prog_id is
// emitted only when > 0, ell_path only when non-empty. The "exists" op always
// emits exists + stored_name; the "get-source" op always emits source. On
// failure, an error object is emitted with code/message and op-specific
// fields (existing_prog_id for insert exit 5; prog_id + compile_errors for
// exit 8).
nlohmann::ordered_json cliResultToJson(const CliResult& r) {
    ordered_json j;
    // op/type/name are included only when known (omitted for usage errors
    // that occur before the subcommand/args are resolved).
    if (!r.op.empty())      j["op"] = r.op;
    if (!r.typeStr.empty()) j["type"] = r.typeStr;
    if (!r.name.empty())    j["name"] = r.name;
    j["success"]    = r.success;
    j["exit_code"]  = r.exitCode;

    if (r.success) {
        if (r.progId > 0)     j["prog_id"]  = r.progId;
        if (!r.ellPath.empty()) j["ell_path"] = r.ellPath;
        // "exists" op: emit the exists boolean (found vs not-found, both exit 0).
        if (r.op == "exists") {
            j["exists"] = r.exists;
            j["stored_name"] = r.storedName;
        }
        // "get-source" op: emit the retrieved EL source text.
        if (r.op == "get-source") j["source"] = r.sourceText;
        // "dependents" op: emit the callers array (M.4b.1). Each entry has
        // name, type (category), and prog_id. An empty array is a valid
        // result (the item has no callers).
        if (r.op == "dependents") {
            j["stored_name"] = r.storedName;
            ordered_json arr = ordered_json::array();
            for (const auto& c : r.callers) {
                ordered_json entry;
                entry["name"]    = c.name;
                entry["type"]    = c.category;
                entry["prog_id"] = c.progId;
                arr.push_back(entry);
            }
            j["dependents"] = arr;
        }
        // Extra top-level payload (serve cache ops): merge each member so
        // the response carries the standalone --json cache/backup/restore
        // object verbatim.
        if (r.extraPayload.is_object()) {
            for (auto it = r.extraPayload.begin();
                 it != r.extraPayload.end(); ++it) {
                j[it.key()] = it.value();
            }
        }
    } else {
        ordered_json err;
        err["code"]    = errorCodeString(r.exitCode, r.op);
        err["message"] = r.errorMessage;
        // exit 5 (insert): include the existing ProgId.
        if (r.exitCode == 5 && r.op == "insert" && r.existingProgId > 0) {
            err["existing_prog_id"] = r.existingProgId;
        }
        // exit 8 (compile error): include the ProgId and raw compiler errors.
        if (r.exitCode == 8) {
            if (r.progId > 0) err["prog_id"] = r.progId;
            err["compile_errors"] = r.compileErrors;
        }
        // TVS-L.10: secondary create-path cleanup error. Emitted only when
        // non-empty — i.e. when partial-entry cleanup was attempted after a
        // post-createEntry failure AND the cleanup itself failed. This is
        // attached SECONDARY to the primary error.code/message; it never
        // replaces them and never flips success. A successful cleanup is
        // silent (no field), so a cleanup failure is always visible.
        if (!r.cleanupError.empty()) {
            err["cleanup_error"] = r.cleanupError;
        }
        j["error"] = err;
    }

    return j;
}

// --- TVS A: batch progress events -------------------------------------------
//
// Batch progress is always on (independent of -v) and goes to stderr only —
// stdout carries the JSON array only under --json. One event per physical
// line: a batch_item_completed for every attempted item (in input order,
// including pre-validation and init failures), then one batch_completed
// after the end-of-batch closeWarehouse() + result rewrite. Human rendering
// by default; --json selects one compact JSON object per line and adds a
// batch_item_started before each completion (the client pairs them). The
// same renderers serve tagged broker requests.

// jsonQuote renders a string as a JSON string literal ("..." with escaping),
// so type/name/message values embedded in a human line can never leak a raw
// quote, backslash, CR, or LF — one event stays one physical line.
std::string jsonQuote(const std::string& s) {
    return ordered_json(s).dump(-1, ' ', false,
                                nlohmann::json::error_handler_t::replace);
}

// Render one batch_item_started event. Returns exactly one line (no CR/LF,
// no trailing newline).
std::string batchItemStartedLine(const std::string& op, size_t index,
                                 size_t total, const std::string& type,
                                 const std::string& name, bool jsonOutput) {
    if (jsonOutput) {
        ordered_json j;
        j["event"]           = "batch_item_started";
        j["batch_operation"] = op;
        j["index"]           = index;
        j["total"]           = total;
        j["type"]            = type;
        j["name"]            = name;
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    return "BATCH START [" + std::to_string(index) + "/" +
           std::to_string(total) + "] op=" + op +
           " type=" + jsonQuote(type) + " name=" + jsonQuote(name);
}

// Render one batch_item_completed event. The JSON form embeds the exact
// cliResultToJson object as "result". The human FAILED line omits message=
// only when the error message is empty.
std::string batchItemCompletedLine(const std::string& op, size_t index,
                                   size_t total, const CliResult& result,
                                   bool jsonOutput) {
    if (jsonOutput) {
        ordered_json j;
        j["event"]           = "batch_item_completed";
        j["batch_operation"] = op;
        j["index"]           = index;
        j["total"]           = total;
        j["type"]            = result.typeStr;
        j["name"]            = result.name;
        j["success"]         = result.success;
        j["exit_code"]       = result.exitCode;
        j["result"]          = cliResultToJson(result);
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    std::string line =
        std::string(result.success ? "BATCH SUCCESS [" : "BATCH FAILED [") +
        std::to_string(index) + "/" + std::to_string(total) + "] op=" + op +
        " type=" + jsonQuote(result.typeStr) +
        " name=" + jsonQuote(result.name) +
        " exit_code=" + std::to_string(result.exitCode);
    if (!result.success && !result.errorMessage.empty()) {
        line += " message=" + jsonQuote(result.errorMessage);
    }
    return line;
}

// Render the single batch_completed summary. cancelled inserts the CANCELLED
// token in the human form; aborted (WHServer VAS guard) inserts the ABORTED
// token plus the machine-readable aborted/abort_reason fields. A non-aborted
// run stays byte-identical to the pre-guard contract: the aborted/abort_reason
// members exist ONLY when aborted=true.
std::string batchCompletedLine(const std::string& op, size_t completed,
                               size_t total, size_t successes,
                               size_t failures, size_t notRun, bool cancelled,
                               bool closeFailed, int exitCode,
                               bool jsonOutput, bool aborted = false,
                               const std::string& abortReason = "") {
    if (jsonOutput) {
        ordered_json j;
        j["event"]           = "batch_completed";
        j["batch_operation"] = op;
        j["completed"]       = completed;
        j["total"]           = total;
        j["successes"]       = successes;
        j["failures"]        = failures;
        j["not_run"]         = notRun;
        j["cancelled"]       = cancelled;
        if (aborted) {
            j["aborted"]      = true;
            j["abort_reason"] = abortReason;
        }
        j["close_failed"]    = closeFailed;
        j["exit_code"]       = exitCode;
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    const char* head = cancelled ? "BATCH COMPLETE CANCELLED"
                     : aborted   ? "BATCH COMPLETE ABORTED"
                                 : "BATCH COMPLETE";
    std::string line = std::string(head) +
        " op=" + op +
        " completed=" + std::to_string(completed) +
        " total=" + std::to_string(total) +
        " successes=" + std::to_string(successes) +
        " failures=" + std::to_string(failures) +
        " not_run=" + std::to_string(notRun) +
        " cancelled=" + (cancelled ? "true" : "false");
    if (aborted) {
        line += " aborted=true abort_reason=" + abortReason;
    }
    return line +
        " close_failed=" + (closeFailed ? "true" : "false") +
        " exit_code=" + std::to_string(exitCode);
}

// Aggregate batch exit code: 1 = batch did not run (pre-run failure),
// 14 = aborted by the WHServer VAS-headroom guard, 3 = cooperatively
// cancelled, 2 = ran with failures, 0 = clean. aborted and cancelled are
// mutually exclusive in runBatch (only one gate can break the loop first);
// aborted is checked first so a hypothetical both-set run reports the
// louder code.
int batchAggregateExitCode(bool batchStarted, bool anyFailed,
                           bool cancelled, bool aborted = false) {
    if (!batchStarted) return 1;
    if (aborted)       return 14;
    if (cancelled)     return 3;
    if (anyFailed)     return 2;
    return 0;
}

// emitBatchProgress writes one rendered event line to stderr and flushes —
// direct writes, no reliance on buffering mode, so each event is visible
// immediately while stdout carries only the final JSON array (--json only).
void emitBatchProgress(const std::string& line) {
    std::fwrite(line.c_str(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// --- --pbar: pinned progress-bar renderer -----------------------------------
//
// Alternative stderr progress rendering for batch runs: a single fixed-width
// Unicode bar line is pinned at the bottom of the output and redrawn in
// place while event lines keep scrolling above it (via eventLine()). stderr
// only — stdout (results / JSON array) is never touched.
//
// Mechanics — deliberately ANSI-free, so it works on any Windows console and
// degrades harmlessly on redirected streams:
//   - Draw: '\r' returns to column 0, the line text is written, then spaces
//     pad out to the widest line drawn so far (maxCells), so a shorter
//     redraw fully overwrites a longer previous line.
//   - Erase: '\r' + maxCells spaces + '\r' — leaves the cursor at column 0
//     of a blank line so an event line can be written on it and scroll up.
//   - Cell width: every emitted glyph (ASCII, U+2588 '█', U+2591 '░',
//     U+2014 '—') occupies exactly one console cell; width is counted in
//     display cells as bytes with (b & 0xC0) != 0x80 (UTF-8 continuation
//     bytes don't count).
//   - Write path: when stderr is a real console (_isatty(_fileno) AND
//     GetConsoleMode(STD_ERROR_HANDLE) both succeed), text is converted
//     UTF-8 -> UTF-16 via MultiByteToWideChar and written via WriteConsoleW
//     (required for the block/shade/em-dash glyphs to render); stderr is
//     fflushed FIRST so buffered C-stdio output can never overtake the
//     direct console writes. Otherwise (pipe/file redirect) the UTF-8 bytes
//     go through fwrite. stderr is fflushed after every write.
//
// pbarLine / pbarEraseSeq / pbarDrawSeq are PURE — no I/O — so
// tests/test_pbar.cpp can pin their exact bytes; PbarRenderer is the thin
// emitter glued on top.

// Maximum display width of a bar line and the fixed width of the bracketed
// bar interior, both in console cells.
const size_t kPbarMaxCells = 79;
const size_t kPbarBarCells = 30;

// UTF-8 encodings of the bar/failure glyphs, spelled as \x escapes so the
// string literals stay ASCII-only regardless of the compiler source charset
// (no /utf-8 flag); literal glyphs in comments are harmless.
const char* const kPbarFullGlyph  = "\xE2\x96\x88";  // U+2588 FULL BLOCK
const char* const kPbarShadeGlyph = "\xE2\x96\x91";  // U+2591 LIGHT SHADE
const char* const kPbarEmDash     = "\xE2\x80\x94";  // U+2014 EM DASH

// Display-cell width of a UTF-8 string under the pinned rule: count bytes
// that are not UTF-8 continuation bytes.
size_t pbarCellWidth(const std::string& s) {
    size_t cells = 0;
    for (unsigned char b : s) {
        if ((b & 0xC0) != 0x80) ++cells;
    }
    return cells;
}

// The longest prefix of `s` occupying at most `maxCells` display cells.
// Never splits a UTF-8 code point (a byte-naive cut could emit a lone lead
// byte); the grouping matches pbarCellWidth exactly, so a stray continuation
// byte counts as 0 cells in both.
std::string pbarTruncCells(const std::string& s, size_t maxCells) {
    size_t cells = 0;
    size_t i = 0;
    while (i < s.size() && cells < maxCells) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
            ++i;  // continuation byte — 0 cells, same as pbarCellWidth
            continue;
        }
        ++cells;
        ++i;
        while (i < s.size() &&
               (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
            ++i;
        }
    }
    return s.substr(0, i);
}

// Returns the complete bar line text as UTF-8 (no '\r', no '\n', no ANSI,
// no trailing spaces). The line NEVER exceeds kPbarMaxCells display cells.
// Pinned format:
//   <op> [<bar>] <done>/<total> <pct>%[ <type> "<name>"][ — <n> failed]
// <bar> is exactly kPbarBarCells cells: U+2588 '█' repeated
// floor(30*done/total) — clamped to a full bar when done > total — then
// U+2591 '░' fill; total == 0 -> all '░' and pct 0. <pct> is
// done*100/total integer, right-aligned to width 3 + '%' (the field widens
// naturally past 999). The label is `<type> "<name>"` with the name through
// the existing jsonQuote() helper — empty when type and name are both
// empty, and truncated with a trailing "..." so the whole line stays within
// kPbarMaxCells. The ` — <n> failed` suffix (U+2014 em dash) appears only
// when failures > 0 and is part of the width budget.
std::string pbarLine(const std::string& op, size_t done, size_t total,
                     const std::string& type, const std::string& name,
                     size_t failures) {
    // Bar fill and percentage are computed in 64-bit so absurdly large
    // counts cannot wrap the (32-bit) size_t intermediate.
    size_t filled = 0;
    unsigned long long pct = 0;
    if (total != 0) {
        const unsigned long long f =
            (static_cast<unsigned long long>(done) * kPbarBarCells) / total;
        filled = static_cast<size_t>(f > kPbarBarCells ? kPbarBarCells : f);
        pct = (static_cast<unsigned long long>(done) * 100) / total;
    }

    std::string line = op;
    line += " [";
    for (size_t i = 0; i < filled; ++i)             line += kPbarFullGlyph;
    for (size_t i = filled; i < kPbarBarCells; ++i) line += kPbarShadeGlyph;
    line += "] ";
    line += std::to_string(done);
    line += '/';
    line += std::to_string(total);
    line += ' ';
    char pctBuf[16];
    std::snprintf(pctBuf, sizeof(pctBuf), "%3llu", pct);
    line += pctBuf;
    line += '%';

    // Optional failures suffix — part of the width budget, appended after
    // the label.
    std::string suffix;
    if (failures > 0) {
        suffix += ' ';
        suffix += kPbarEmDash;
        suffix += ' ';
        suffix += std::to_string(failures);
        suffix += " failed";
    }

    // Optional `<type> "<name>"` label — the non-empty pieces are joined so
    // a type-only or name-only item still renders something sensible.
    std::string label = type;
    if (!name.empty()) {
        if (!label.empty()) label += ' ';
        label += jsonQuote(name);
    }

    if (!label.empty()) {
        const size_t used = pbarCellWidth(line) + pbarCellWidth(suffix);
        if (used < kPbarMaxCells) {
            // Cells available for ' ' + label (or ' ' + truncated + "...").
            const size_t budget = kPbarMaxCells - used;
            if (budget >= 1 + pbarCellWidth(label)) {
                line += ' ';
                line += label;
            } else if (budget >= 4) {
                // Minimum useful form: ' ' + "...". The content before the
                // ellipsis shrinks to whatever fits (possibly nothing).
                line += ' ';
                line += pbarTruncCells(label, budget - 4);
                line += "...";
            }
            // else: not even ' ...' fits — the label is dropped entirely.
        }
    }
    line += suffix;

    // Hard guarantee: the pathological case where op + counts + suffix
    // alone exceed the width budget still cannot emit an over-wide line.
    // The cut can land on a separator/pad space, so strip trailing spaces
    // to keep the pinned no-trailing-space invariant (the line is
    // non-empty here — the clamp only runs on over-wide input).
    if (pbarCellWidth(line) > kPbarMaxCells) {
        line = pbarTruncCells(line, kPbarMaxCells);
        while (!line.empty() && line.back() == ' ') line.pop_back();
    }
    return line;
}

// Returns the erase sequence for a previously drawn bar occupying `cells`
// display cells: '\r' + <cells spaces> + '\r'.
std::string pbarEraseSeq(int cells) {
    std::string out = "\r";
    if (cells > 0) out.append(static_cast<size_t>(cells), ' ');
    out += '\r';
    return out;
}

// Returns the draw sequence for `line`: '\r' + line + trailing spaces so
// the total display width equals `cells` (already-wide lines get no pad).
// The caller passes the running max line width so shorter redraws fully
// overwrite longer previous ones.
std::string pbarDrawSeq(const std::string& line, int cells) {
    std::string out = "\r";
    out += line;
    const int pad = cells - static_cast<int>(pbarCellWidth(line));
    if (pad > 0) out.append(static_cast<size_t>(pad), ' ');
    return out;
}

// PbarRenderer — the --pbar emitter. Thin glue over the pure helpers: it
// tracks the widest line drawn (maxCells) so every erase/overwrite is
// complete, and routes bytes through WriteConsoleW on a real console or
// fwrite on a redirected stream. The bar itself never emits event lines —
// callers route BATCH/other event text through eventLine() so it scrolls
// above the pinned bar.
struct PbarRenderer {
    PbarRenderer(const std::string& op, size_t total)
        : op_(op), total_(total) {
        // Console iff stderr is a character device AND still resolves to a
        // console handle — the pair rejects pipes/files even when a console
        // is attached elsewhere.
        const int fd = _fileno(stderr);
        DWORD mode = 0;
        isConsole_ = (fd >= 0) && (_isatty(fd) != 0) &&
                     GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode);
    }

    // Draw the initial all-empty bar (no newline — the bar stays pinned).
    void start() {
        done_ = 0;
        failures_ = 0;
        type_.clear();
        name_.clear();
        draw();
    }

    // Point the label at the item now being attempted and redraw. Emits no
    // event line — callers emit those separately (or via eventLine()).
    void itemStarted(const std::string& type, const std::string& name) {
        type_ = type;
        name_ = name;
        erase();
        draw();
    }

    void itemCompleted() {
        ++done_;
        draw();
    }

    void noteFailure() {
        ++failures_;
        draw();
    }

    // Write `line` on the bar's line so it scrolls up, then redraw the bar
    // underneath: erase -> event line + '\n' -> redraw.
    void eventLine(const std::string& line) {
        erase();
        writeRaw(line);
        writeRaw("\n");
        draw();
    }

    // Final draw + '\n' — the bar becomes a permanent last-but-summary
    // line in the scrollback.
    void finish() {
        draw();
        writeRaw("\n");
    }

private:
    // Redraw the current bar line, padding out to maxCells so the previous
    // (possibly wider) line is fully overwritten.
    void draw() {
        const std::string line =
            pbarLine(op_, done_, total_, type_, name_, failures_);
        const int cells = static_cast<int>(pbarCellWidth(line));
        if (cells > maxCells_) maxCells_ = cells;
        writeRaw(pbarDrawSeq(line, maxCells_));
    }

    // Clear the whole bar line and leave the cursor at column 0.
    void erase() {
        if (maxCells_ > 0) writeRaw(pbarEraseSeq(maxCells_));
    }

    // Raw stderr write. On a real console the text is widened and written
    // through WriteConsoleW (fflush first so C-stdio and console writes
    // stay ordered); on a redirected stream the UTF-8 bytes go through
    // fwrite. stderr is always fflushed afterwards — even for an empty
    // write. Both API calls are best-effort: conversion/write failures
    // simply drop the piece (progress cosmetics must never take down the
    // run).
    void writeRaw(const std::string& utf8) {
        if (!utf8.empty()) {
            if (isConsole_) {
                std::fflush(stderr);
                const int wlen = MultiByteToWideChar(
                    CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                    nullptr, 0);
                if (wlen > 0) {
                    std::wstring w(static_cast<size_t>(wlen), L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                        static_cast<int>(utf8.size()),
                                        w.data(), wlen);
                    DWORD written = 0;
                    WriteConsoleW(GetStdHandle(STD_ERROR_HANDLE), w.data(),
                                  static_cast<DWORD>(w.size()), &written,
                                  nullptr);
                }
            } else {
                std::fwrite(utf8.data(), 1, utf8.size(), stderr);
            }
        }
        std::fflush(stderr);
    }

    std::string op_;
    size_t total_ = 0;
    size_t done_ = 0;
    size_t failures_ = 0;
    std::string type_;   // current item label pieces
    std::string name_;
    int  maxCells_ = 0;      // widest line drawn so far (display cells)
    bool isConsole_ = false; // stderr resolves to a real console
};

// --pbar log routing: while a batch runs under --pbar, tsdev_lib's
// level-gated diagnostics are redirected through pbarLogSink so each
// message scrolls above the pinned bar as an event line instead of
// corrupting the redraw. g_pbarLogTarget points at the worker's live
// PbarRenderer; both are installed where bar.start() runs and cleared
// before the worker exits (single-op bars are drawn post-hoc and need no
// sink).
PbarRenderer* g_pbarLogTarget = nullptr;
void pbarLogSink(const char* s) {
    std::string line = (s != nullptr) ? s : "";
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    if (!line.empty() && g_pbarLogTarget) {
        g_pbarLogTarget->eventLine(line);
    }
}

// §5 teardown (dispatch_watchdog.cpp:79-88 convention — the proven field
// incident): a process that may still hold an OPEN warehouse session must
// NEVER `delete lib` or ExitProcess — orlib20's DLL detach then raises the
// BLOCKING "WHouse stays in memory" dialog (an INVISIBLE deadlock under
// the SW_HIDE serve broker, a hard block for a direct CLI). Pending output
// is emitted by the caller first; this helper flushes both streams and
// dies via TerminateProcess — the only exit that skips ALL DLL detach.
[[noreturn]] void terminateSelfNoDetach(unsigned exitCode) {
    std::fflush(stdout);
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exitCode));
    // TerminateProcess does not return; unreachable.
    std::abort();
}

// emitOutput renders a CliResult in either human-readable or JSON form.
//
// Human mode (jsonMode == false): writes humanStdout to stdout and
// humanStderr to stderr exactly as captured (byte-identical to the
// pre-JSON behavior). At most one of the two is non-empty per result.
//
// JSON mode (jsonMode == true): emits exactly ONE JSON object on stdout
// followed by a single newline, and suppresses the human-readable stdout
// and stderr messages. The library's own stderr diagnostics (e.g.
// "TS install path resolved...") are emitted by tsdev_lib.cpp through the
// runtime-gated log levels (default: warnings + errors only; -v/-vv raise
// to info/debug) and are NOT part of the result output. nlohmann/json
// performs all escaping (backslashes in Windows paths, quotes, newlines)
// automatically.
void emitOutput(const CliResult& r, bool jsonMode) {
    try {
    if (!jsonMode) {
        if (!r.humanStdout.empty()) {
            std::fwrite(r.humanStdout.c_str(), 1, r.humanStdout.size(), stdout);
            std::fflush(stdout);
        }
        if (!r.humanStderr.empty()) {
            std::fwrite(r.humanStderr.c_str(), 1, r.humanStderr.size(), stderr);
            std::fflush(stderr);
        }
        return;
    }

    ordered_json j = cliResultToJson(r);
    const std::string s = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    std::fwrite(s.c_str(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    } catch (...) {
        // Belt-and-suspenders: even with the replace error handler on dump(),
        // a future code path could throw during output serialization. Emit a
        // JSON error and exit cleanly instead of crashing (which would launch
        // TSCrashReport.exe via the unhandled-exception filter).
        const char* errMsg =
            "{\"op\":\"unknown\",\"success\":false,\"exit_code\":1,"
            "\"error\":{\"code\":\"internal_error\","
            "\"message\":\"internal error during output serialization\"}}\n";
        std::fwrite(errMsg, 1, std::strlen(errMsg), stdout);
        std::fflush(stdout);
        // TerminateProcess(1), NOT std::exit(1)/ExitProcess: emitOutput also
        // runs on the session-owning worker of a §5 terminate path, where
        // the warehouse session may still be open — ExitProcess would
        // DLL-detach into it (the "WHouse stays in memory" modal). On every
        // other call site the lib is already destructed, so the teardown is
        // observably identical either way: same error JSON, same exit 1.
        terminateSelfNoDetach(1);
    }
}

// §5 teardown for the single-op workers (insert/delete/upsert — the only
// ops whose per-item flow includes a closeWarehouse). When the result
// flags a possibly-open session (the close that this op owned did not
// provably complete — see CliResult::sessionMayBeOpen), emit the pending
// output ON THIS WORKER — the main thread never runs again — then flush +
// TerminateProcess with the result's own exit code. NEVER `delete lib` or
// return into ExitProcess. The emitted bytes are identical to the cmd*
// tail's: the post-hoc --pbar draw, then emitOutput. On this path the
// result is always a failure (the flag only rides failure results), so the
// bar draws the "— 1 failed" form and emitOutput always runs — mirroring
// the cmd* tail exactly; only the teardown differs. No-op when the flag
// is unset.
void singleOpTerminateIfSessionOpen(const CliResult& r,
                                    const std::string& typeStr,
                                    const std::string& name,
                                    bool jsonMode, bool pbar) {
    if (!r.sessionMayBeOpen) return;
    if (pbar) {
        PbarRenderer bar(r.op, 1);
        bar.itemStarted(typeStr, name);
        bar.noteFailure();
        bar.itemCompleted();
        bar.finish();
    }
    emitOutput(r, jsonMode);
    terminateSelfNoDetach(static_cast<unsigned>(r.exitCode));
}

// --- --backup-cache / --restore-cache: cache phase ---------------------------
//
// Global, position-free presence flags. The cache phase runs once, after all
// argument validation and the elevation guard but BEFORE any subcommand
// dispatch, in the fixed order backup -> restore -> subcommand regardless of
// argv order (the serve subcommand rejects the flags earlier). Any
// cache-phase failure aborts the run before the subcommand — the flags are
// an insurance contract and never silently proceed.
//
// Rendering mirrors the --pbar mechanics: pure line builders
// (cacheElapsedFormat / cacheProgressLine / cacheSpinnerLine /
// cachePipeProgressLine) so a same-TU unit test can pin the bytes, plus a
// thin CacheProgressRenderer driven inline by the engine's CacheProgress
// callback (no display thread). Console stderr gets a redrawn <=79-cell
// frame throttled to >=100 ms; redirected stderr gets one whole line every
// >=5 s (NDJSON under --json). All of it is stderr-only — with a subcommand
// the cache phase writes nothing to stdout; standalone
// (--backup-cache/--restore-cache with no subcommand) prints a stdout
// result (human COMPLETE/KEPT lines, or one JSON object under --json).

// --cache-backup-policy value -> parsed enum; Prompt is the default.
enum class CacheBackupPolicy { Prompt, Overwrite, Skip, Abort };

// Case-insensitive value parse. False on an unrecognized value (usage error).
bool parseCacheBackupPolicy(const std::string& v, CacheBackupPolicy& out) {
    if (iequals(v, "prompt"))    { out = CacheBackupPolicy::Prompt;    return true; }
    if (iequals(v, "overwrite")) { out = CacheBackupPolicy::Overwrite; return true; }
    if (iequals(v, "skip"))      { out = CacheBackupPolicy::Skip;      return true; }
    if (iequals(v, "abort"))     { out = CacheBackupPolicy::Abort;     return true; }
    return false;
}

// Format an elapsed duration as M:SS below one hour, else H:MM:SS. Pure.
std::string cacheElapsedFormat(double elapsedSecs) {
    unsigned long long s =
        elapsedSecs > 0.0 ? static_cast<unsigned long long>(elapsedSecs) : 0;
    char buf[32];
    if (s < 3600) {
        std::snprintf(buf, sizeof(buf), "%llu:%02llu", s / 60, s % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu",
                      s / 3600, (s / 60) % 60, s % 60);
    }
    return buf;
}

// Determinate console frame (pure, no '\r'/'\n', never wider than
// kPbarMaxCells display cells):
//   <OP> [<30-cell bar>] <pct>% <elapsed>[ — <current file basename>]
// Same bar geometry as pbarLine: kPbarBarCells of '█'/'░', pct = done*100/
// total right-aligned to width 3 + '%', elapsed via cacheElapsedFormat. The
// " — <file>" suffix (U+2014 em dash) is truncated on a code-point boundary
// to fit, and dropped entirely when fewer than 4 cells remain.
std::string cacheProgressLine(const std::string& op, uint64_t done,
                              uint64_t total, double elapsedSecs,
                              const std::string& fileBase) {
    size_t filled = 0;
    unsigned long long pct = 0;
    if (total != 0) {
        const unsigned long long f =
            (static_cast<unsigned long long>(done) * kPbarBarCells) / total;
        filled = static_cast<size_t>(f > kPbarBarCells ? kPbarBarCells : f);
        pct = (static_cast<unsigned long long>(done) * 100) / total;
    }
    std::string line = op;
    line += " [";
    for (size_t i = 0; i < filled; ++i)             line += kPbarFullGlyph;
    for (size_t i = filled; i < kPbarBarCells; ++i) line += kPbarShadeGlyph;
    line += "] ";
    char pctBuf[16];
    std::snprintf(pctBuf, sizeof(pctBuf), "%3llu", pct);
    line += pctBuf;
    line += '%';
    line += ' ';
    line += cacheElapsedFormat(elapsedSecs);

    if (!fileBase.empty()) {
        const size_t used = pbarCellWidth(line);
        if (used < kPbarMaxCells) {
            const size_t budget = kPbarMaxCells - used;  // cells for suffix
            if (budget >= 3 + pbarCellWidth(fileBase)) {
                line += ' ';
                line += kPbarEmDash;
                line += ' ';
                line += fileBase;
            } else if (budget >= 4) {
                // Minimum useful form: " — " + a truncated basename.
                line += ' ';
                line += kPbarEmDash;
                line += ' ';
                line += pbarTruncCells(fileBase, budget - 3);
            }
            // else: the suffix is dropped entirely.
        }
    }
    if (pbarCellWidth(line) > kPbarMaxCells) {
        line = pbarTruncCells(line, kPbarMaxCells);
        while (!line.empty() && line.back() == ' ') line.pop_back();
    }
    return line;
}

// Indeterminate console frame (pure, ASCII-only): "<OP> <spin> <elapsed>"
// where <spin> cycles '|', '/', '-', '\' by tick.
std::string cacheSpinnerLine(const std::string& op, unsigned tick,
                             double elapsedSecs) {
    const char kSpin[] = "|/-\\";
    std::string line = op;
    line += ' ';
    line += kSpin[tick % 4];
    line += ' ';
    line += cacheElapsedFormat(elapsedSecs);
    return line;
}

// Redirected-stderr progress line (pure): the human form is a whole
// key=value line; under --json it is one NDJSON event object
// (cache_backup_progress / cache_restore_progress by op name).
std::string cachePipeProgressLine(const std::string& op, uint64_t done,
                                  uint64_t total, double elapsedSecs,
                                  bool jsonMode) {
    const unsigned long long pct =
        total != 0 ? (static_cast<unsigned long long>(done) * 100) / total : 0;
    const unsigned long long es =
        elapsedSecs > 0.0 ? static_cast<unsigned long long>(elapsedSecs) : 0;
    if (jsonMode) {
        ordered_json j;
        j["event"] = (op == "RESTORE") ? "cache_restore_progress"
                                       : "cache_backup_progress";
        j["bytes"]        = done;
        j["total"]        = total;
        j["pct"]          = pct;
        j["elapsed_secs"] = es;
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    return op + " PROGRESS pct=" + std::to_string(pct) +
           " bytes=" + std::to_string(done) +
           " total=" + std::to_string(total) +
           " elapsed_secs=" + std::to_string(es);
}

// Wide -> UTF-8 conversion for paths/messages (the CLI's argv-derived strings
// are narrow; engine paths are wide). Empty/failed conversion -> "".
std::string wideToUtf8Cli(const std::wstring& s) {
    if (s.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                  static_cast<int>(s.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &out[0], len, nullptr, nullptr);
    return out;
}

// Byte count -> "<value> <unit>" with two decimals (KiB/MiB/GiB/TiB) for the
// existing-backup prompt's size= field. Pure.
std::string formatBytesHuman(uint64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    size_t u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[48];
    if (u == 0) {
        std::snprintf(buf, sizeof(buf), "%llu B",
                      static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f %s", v, units[u]);
    }
    return buf;
}

// stderr resolves to a real console (same detection pair as PbarRenderer).
bool stderrIsConsole() {
    const int fd = _fileno(stderr);
    DWORD mode = 0;
    return (fd >= 0) && (_isatty(fd) != 0) &&
           GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode);
}

// CONIN$ can be opened — the second half of the prompt-interactivity check.
// Opening CONIN$ (not stdin) keeps the prompt independent of a piped stdin
// (--batch-stdin / --batch-progress-control own stdin).
bool coninAvailable() {
    HANDLE hIn = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (hIn == INVALID_HANDLE_VALUE) return false;
    CloseHandle(hIn);
    return true;
}

// CacheProgressRenderer — the cache-phase emitter. Thin glue over the pure
// builders: the engine's CacheProgress callback calls progress() inline (no
// display thread); console stderr gets a '\r'-redrawn frame throttled to
// >=100 ms, redirected stderr gets a whole line every >=5 s. finish()
// always emits the final frame + '\n'. Determinate rendering (bar) is used
// whenever the engine reports a nonzero byte total; otherwise the ASCII
// spinner is drawn.
struct CacheProgressRenderer {
    CacheProgressRenderer(const std::string& op, bool jsonMode)
        : op_(op), json_(jsonMode) {
        isConsole_ = stderrIsConsole();
        start_ = std::chrono::steady_clock::now();
        // The first progress callback always draws.
        lastDraw_ = start_ - throttle();
    }

    // Engine callback entry point (CacheProgress signature).
    void progress(uint64_t done, uint64_t total, const std::wstring& file) {
        lastDone_ = done;
        lastTotal_ = total;
        if (!file.empty()) {
            const size_t sep = file.find_last_of(L"\\/");
            fileBase_ = wideToUtf8Cli(
                sep == std::wstring::npos ? file : file.substr(sep + 1));
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastDraw_ < throttle()) return;
        lastDraw_ = now;
        drawFrame();
    }

    // Final frame + newline (a whole PROGRESS/NDJSON line on a pipe).
    void finish() {
        drawFrame();
        writeRaw("\n");
    }

private:
    std::chrono::steady_clock::duration throttle() const {
        return isConsole_ ? std::chrono::milliseconds(100)
                          : std::chrono::seconds(5);
    }

    double elapsed() const {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - start_)
            .count();
    }

    void drawFrame() {
        if (isConsole_) {
            const std::string line =
                lastTotal_ != 0
                    ? cacheProgressLine(op_, lastDone_, lastTotal_, elapsed(),
                                        fileBase_)
                    : cacheSpinnerLine(op_, tick_++, elapsed());
            const int cells = static_cast<int>(pbarCellWidth(line));
            if (cells > maxCells_) maxCells_ = cells;
            writeRaw(pbarDrawSeq(line, maxCells_));
        } else {
            writeRaw(cachePipeProgressLine(op_, lastDone_, lastTotal_,
                                           elapsed(), json_));
            writeRaw("\n");
        }
    }

    // Same write path as PbarRenderer::writeRaw — WriteConsoleW on a real
    // console (fflush first so C-stdio and console writes stay ordered),
    // UTF-8 fwrite on a redirected stream. Best-effort: conversion/write
    // failures drop the piece; stderr is fflushed afterwards regardless.
    void writeRaw(const std::string& utf8) {
        if (!utf8.empty()) {
            if (isConsole_) {
                std::fflush(stderr);
                const int wlen = MultiByteToWideChar(
                    CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                    nullptr, 0);
                if (wlen > 0) {
                    std::wstring w(static_cast<size_t>(wlen), L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                        static_cast<int>(utf8.size()),
                                        w.data(), wlen);
                    DWORD written = 0;
                    WriteConsoleW(GetStdHandle(STD_ERROR_HANDLE), w.data(),
                                  static_cast<DWORD>(w.size()), &written,
                                  nullptr);
                }
            } else {
                std::fwrite(utf8.data(), 1, utf8.size(), stderr);
            }
        }
        std::fflush(stderr);
    }

    std::string op_;            // "BACKUP" / "RESTORE"
    bool json_ = false;         // --json selects NDJSON on redirected stderr
    bool isConsole_ = false;
    uint64_t lastDone_ = 0;
    uint64_t lastTotal_ = 0;
    std::string fileBase_;      // current-file basename (UTF-8)
    unsigned tick_ = 0;         // spinner position
    int maxCells_ = 0;          // widest console frame drawn
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point lastDraw_;
};

// Outcome of the existing-backup policy decision. Prompt = ask the operator
// via CONIN$ (only reachable on an interactive console); Overwrite = create
// or replace the backup; Skip = keep the existing backup and continue;
// Cancel = abort the run with exit 17.
enum class BackupAction { Prompt, Overwrite, Skip, Cancel };

// Pure policy decision for an existing (or absent) cache backup:
//   - !exists               -> Overwrite (proceed silently)
//   - exists && !complete   -> Overwrite regardless of policy EXCEPT abort,
//     which still cancels -> Cancel. A torn/corrupt backup.info is never
//     offered "keep existing": keeping broken insurance is the unsafe
//     default. The caller prints the incomplete-backup warning.
//   - exists && complete    -> overwrite->Overwrite, skip->Skip,
//     abort->Cancel, prompt->Prompt when `interactive` else Skip (the
//     non-interactive prompt fallback; caller prints the keep-warning).
BackupAction decideBackupAction(const BackupInfo& info,
                                CacheBackupPolicy policy,
                                bool interactive) {
    if (!info.exists) return BackupAction::Overwrite;
    if (!info.complete) {
        return policy == CacheBackupPolicy::Abort ? BackupAction::Cancel
                                                  : BackupAction::Overwrite;
    }
    switch (policy) {
        case CacheBackupPolicy::Overwrite: return BackupAction::Overwrite;
        case CacheBackupPolicy::Skip:      return BackupAction::Skip;
        case CacheBackupPolicy::Abort:     return BackupAction::Cancel;
        case CacheBackupPolicy::Prompt:
            return interactive ? BackupAction::Prompt : BackupAction::Skip;
    }
    return BackupAction::Overwrite;
}

// Pure builder for the existing-backup detail line printed above the
// interactive prompt: path=<root>\cache created=<ts> files=<n> size=<human>
// [ source=<info.source>]. `source=` is omitted when the backup carries no
// recorded source dir — it identifies which install the backup was taken
// from so a changed install/--ts-path never blind-overwrites.
std::string existingBackupDetailLine(const std::wstring& backupRoot,
                                     const BackupInfo& info) {
    std::string line = "path=" + wideToUtf8Cli(backupRoot) +
        "\\cache created=" + info.created +
        " files=" + std::to_string(
            static_cast<unsigned long long>(info.files)) +
        " size=" + formatBytesHuman(info.bytes);
    if (!info.source.empty()) {
        line += " source=";
        line += wideToUtf8Cli(info.source);
    }
    return line;
}

// Pure key-from-line parse for the existing-backup prompt: the first
// non-whitespace wide char of `line` (lowercased) is the choice
// ('o'/'s'/'c'); an empty or whitespace-only line (bare Enter) is 's';
// anything else returns L'\0' (invalid -> caller re-prompts).
wchar_t cachePromptChoice(const std::wstring& line) {
    for (wchar_t ch : line) {
        if (iswspace(ch)) continue;
        ch = static_cast<wchar_t>(towlower(ch));
        return (ch == L'o' || ch == L's' || ch == L'c') ? ch : L'\0';
    }
    return L's';
}

// Interactive existing-backup prompt. Returns 'o' (overwrite), 's' (skip —
// keep the existing backup and continue), or 'c' (cancel). Keys are
// case-insensitive; bare Enter is 's'; an invalid key re-prompts; EOF/read
// failure returns 's' (the non-interactive default). Input is read from
// CONIN$ via ReadConsoleW — never stdin, which --batch-stdin /
// --batch-progress-control own — and the console echoes the key itself.
// Callers must only invoke this when stderr is a console AND CONIN$ opens
// (checked again here defensively).
char promptExistingCacheBackup(const std::wstring& backupRoot,
                               const BackupInfo& info) {
    std::fprintf(stderr, "tsdev_cli: a cache backup already exists:\n");
    std::fprintf(stderr, "%s\n",
                 existingBackupDetailLine(backupRoot, info).c_str());
    HANDLE hIn = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (hIn == INVALID_HANDLE_VALUE) return 's';
    char result = 's';
    for (;;) {
        std::fprintf(stderr,
            "[O]verwrite / [S]kip %s keep existing backup, continue / "
            "[C]ancel: ", kPbarEmDash);
        std::fflush(stderr);
        // Read the FULL line per attempt (ReadConsoleW in cooked mode
        // returns through the trailing CR/LF), then take the first
        // non-whitespace char via the pure cachePromptChoice seam. Reading
        // one char at a time left the '\n' queued after an invalid key, so
        // the next read consumed it as a bare Enter and the documented
        // re-prompt never happened.
        wchar_t buf[256];
        DWORD n = 0;
        if (!ReadConsoleW(hIn, buf, _countof(buf), &n, nullptr) || n == 0) {
            std::fputc('\n', stderr);
            result = 's';  // EOF/read failure -> non-interactive default
            break;
        }
        // A line longer than the buffer arrives without its '\n'; drain the
        // remainder so it cannot pollute the next prompt attempt. `line`
        // is captured first so the drain cannot clobber the choice text.
        std::wstring line(buf, n);
        bool sawNewline = line.find(L'\n') != std::wstring::npos;
        while (!sawNewline) {
            DWORD dn = 0;
            if (!ReadConsoleW(hIn, buf, _countof(buf), &dn, nullptr) ||
                dn == 0) {
                break;
            }
            for (DWORD k = 0; k < dn && !sawNewline; ++k)
                sawNewline = (buf[k] == L'\n');
        }
        const wchar_t choice = cachePromptChoice(line);
        if (choice != L'\0') {
            result = static_cast<char>(choice);
            break;
        }
        std::fprintf(stderr, "tsdev_cli: please enter O, S, or C.\n");
    }
    CloseHandle(hIn);
    return result;
}

// Torn-capture disclosure: true when the backup copied at least one file
// from a live/locked source — via a VSS snapshot or the
// FILE_FLAG_BACKUP_SEMANTICS fallback (CacheOpResult::usedVss /
// usedBackupSemantics in cache_backup.h; mirrors backup.info's
// live_source=true marker).
bool backupCapturedLiveSource(const CacheOpResult& res) {
    return res.usedVss || res.usedBackupSemantics;
}

// " live_source=true" suffix for the BACKUP COMPLETE line when the backup
// captured a live source; empty otherwise.
const char* liveSourceSuffix(const CacheOpResult& res) {
    return backupCapturedLiveSource(res) ? " live_source=true" : "";
}

// Additive-only JSON disclosure for the standalone cache/backup payload:
// "live_source":true when the capture touched a live source, plus
// "used_vss":true when the VSS snapshot path was used. No keys otherwise.
void applyLiveSourceJson(ordered_json& c, const CacheOpResult& res) {
    if (backupCapturedLiveSource(res)) c["live_source"] = true;
    if (res.usedVss) c["used_vss"] = true;
}

// The single stdout JSON error object for a cache-phase failure under
// --json. Pure builder sharing the CliResult shape via cliResultToJson:
// {"op":<phase label>,"success":false,"exit_code":<n>,
//  "error":{"code":<derived from exit code>,"message":<message>}}.
// `op` is "backup-cache", "restore-cache", or "backup-cache+restore-cache"
// matching which phase(s) ran (same label as the standalone success object).
ordered_json cacheErrorJson(const std::string& op, int exitCode,
                            const std::string& message) {
    CliResult r;
    r.op           = op;
    r.exitCode     = exitCode;
    r.errorMessage = message;
    return cliResultToJson(r);
}

// Write one cacheErrorJson object to stdout. Human mode is a no-op — the
// stderr "tsdev_cli: ERROR — ..." lines already carry the reason. Called at
// most once per run (every call site is a failure return), so under --json
// stdout still carries exactly one object even in combined mode (the
// subcommand never runs, so nothing else can emit).
//
// Serve-request mode (serveResult != nullptr): no stdout write — the broker
// response frame is a CliResult serialized by writeServeResponse, so the
// failure is recorded into `serveResult` instead (same CliResult shape the
// standalone cacheErrorJson serializes through cliResultToJson).
void emitCacheErrorJson(const std::string& op, int exitCode,
                        const std::string& message, bool jsonMode,
                        CliResult* serveResult = nullptr) {
    if (serveResult) {
        serveResult->op           = op;
        serveResult->success      = false;
        serveResult->exitCode     = exitCode;
        serveResult->errorMessage = message;
        return;
    }
    if (!jsonMode) return;
    const ordered_json j = cacheErrorJson(op, exitCode, message);
    const std::string s =
        j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    std::fwrite(s.c_str(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// The single stdout success object for a standalone cache-phase run under
// --json: {"op":<phase label>,"success":true,"exit_code":0,"cache"|"backup"/
// "restore":{...}}. Pure builder — also used by the serve-request path, which
// hands the same object back via CliResult::extraPayload so the broker
// response mirrors this exact shape.
ordered_json cacheSuccessJson(const std::string& cacheOp,
                              bool doBackup, bool doRestore,
                              bool backupKept,
                              const CacheOpResult& backupRes,
                              const CacheOpResult& restoreRes,
                              const std::wstring& backupRoot,
                              const std::wstring& cacheDir) {
    ordered_json j;
    const bool both = doBackup && doRestore;
    j["op"] = cacheOp;
    j["success"] = true;
    j["exit_code"] = 0;
    if (doBackup) {
        ordered_json c;
        c["path"] = wideToUtf8Cli(backupRoot + L"\\cache");
        if (backupKept) {
            c["skipped"] = true;
            c["created"] = readBackupInfo(backupRoot).created;
        } else {
            c["files"]        = backupRes.files;
            c["bytes"]        = backupRes.bytes;
            c["elapsed_secs"] =
                static_cast<unsigned long long>(backupRes.elapsedSecs);
            c["created"] = readBackupInfo(backupRoot).created;
            applyLiveSourceJson(c, backupRes);
        }
        j[both ? "backup" : "cache"] = c;
    }
    if (doRestore) {
        ordered_json c;
        c["path"]         = wideToUtf8Cli(cacheDir);
        c["files"]        = restoreRes.files;
        c["bytes"]        = restoreRes.bytes;
        c["elapsed_secs"] =
            static_cast<unsigned long long>(restoreRes.elapsedSecs);
        j[both ? "restore" : "cache"] = c;
    }
    return j;
}

// Run the cache phase for --backup-cache / --restore-cache. Returns -1 to
// continue into subcommand dispatch, or the process exit code to return
// immediately (standalone runs and all abort paths).
//
// `subcommand` is empty for a standalone invocation (no subcommand given);
// the "— <subcommand> not run" tails then read "— exiting".
//
// Serve-request mode (serveResult != nullptr): invoked by
// dispatchServeRequest for the "backup-cache"/"restore-cache" broker ops.
// The phase logic is identical (pure filesystem), but nothing is written to
// stdout — the result is recorded into `serveResult` instead: failures go
// through emitCacheErrorJson's serve branch, and on success the standalone
// --json payload object is stored in serveResult->extraPayload so
// cliResultToJson merges it into the response frame. The cancel tail names
// the serve session (the run is not aborted — only the request's backup
// intent is).
int runCachePhase(bool doBackup, bool doRestore, CacheBackupPolicy policy,
                  const wchar_t* tsPathOverride,
                  const std::string& subcommand, bool jsonMode,
                  bool noCacheCompress = false,
                  CliResult* serveResult = nullptr) {
    // Tail for the "cancelled/failed — X" stderr lines: names the skipped
    // subcommand in combined mode, or "exiting" standalone (avoids the
    // double-negative "no further operations not run"). Serve mode notes
    // that the broker session continues.
    const std::string tail =
        serveResult ? "serve session continues"
                    : (subcommand.empty() ? "exiting"
                                          : (subcommand + " not run"));
    // JSON "op" label: which phase(s) were requested (same for the success
    // object and every failure object).
    const std::string cacheOp =
        (doBackup && doRestore) ? "backup-cache+restore-cache"
                                : (doBackup ? "backup-cache" : "restore-cache");

    const std::wstring backupRoot = cacheBackupRoot();
    if (backupRoot.empty()) {
        const std::string msg =
            "cannot determine the cache backup root "
            "(LOCALAPPDATA is unavailable)";
        std::fprintf(stderr, "tsdev_cli: ERROR — %s\n", msg.c_str());
        emitCacheErrorJson(cacheOp, 16, msg, jsonMode, serveResult);
        return 16;
    }
    const std::wstring cacheDir = resolveTsCacheDir(tsPathOverride);
    std::error_code cacheDirEc;
    const bool cacheDirExists =
        !cacheDir.empty() &&
        std::filesystem::exists(std::filesystem::path(cacheDir), cacheDirEc);
    if (cacheDir.empty() || cacheDirEc || !cacheDirExists) {
        const std::string msg =
            "cannot locate TradeStation cache directory (looked for " +
            wideToUtf8Cli(cacheDir) + ")";
        std::fprintf(stderr, "tsdev_cli: ERROR — %s\n", msg.c_str());
        emitCacheErrorJson(cacheOp, 16, msg, jsonMode, serveResult);
        return 16;
    }

    bool backupKept = false;
    CacheOpResult backupRes{};
    if (doBackup) {
        // Warn-only: a running desktop means the copy may capture a live cache.
        const std::vector<std::wstring> running = runningTsProcessNames();
        if (!running.empty()) {
            std::string names;
            for (const std::wstring& p : running) {
                if (!names.empty()) names += ", ";
                names += wideToUtf8Cli(p);
            }
            std::fprintf(stderr,
                "tsdev_cli: WARNING — TradeStation is running (%s); the "
                "backup may capture a live cache.\n", names.c_str());
        }

        // Existing-backup policy decision (pure seam: decideBackupAction).
        const BackupInfo info = readBackupInfo(backupRoot);
        const bool interactive = stderrIsConsole() && coninAvailable();
        const BackupAction action =
            decideBackupAction(info, policy, interactive);
        if (info.exists && !info.complete &&
            action != BackupAction::Cancel) {
            std::fprintf(stderr,
                "tsdev_cli: WARNING — existing cache backup is incomplete "
                "(created=%s) — replacing it with a fresh backup\n",
                info.created.empty() ? "unknown" : info.created.c_str());
        }
        char decision = 'o';
        switch (action) {
            case BackupAction::Cancel:
                decision = 'c';
                break;
            case BackupAction::Prompt:
                decision = promptExistingCacheBackup(backupRoot, info);
                break;
            case BackupAction::Skip:
                decision = 's';
                // The non-interactive prompt fallback keeps the backup with
                // a warning (interactive Prompt never reaches this case).
                if (policy == CacheBackupPolicy::Prompt && !interactive) {
                    std::fprintf(stderr,
                        "tsdev_cli: WARNING — keeping existing backup "
                        "(created=%s) — continuing without a new backup\n",
                        info.created.c_str());
                }
                break;
            case BackupAction::Overwrite:
                decision = 'o';
                break;
        }
        if (decision == 'c') {
            std::fprintf(stderr,
                "tsdev_cli: cache backup cancelled — %s\n",
                tail.c_str());
            // Serve mode carries the "session continues" notion inside the
            // error message itself: the broker's stderr is not captured by
            // the client, so the JSON message is the only channel.
            const std::string cancelMsg =
                serveResult
                    ? "cache backup cancelled (serve session continues)"
                    : "cache backup cancelled";
            emitCacheErrorJson(cacheOp, 17, cancelMsg,
                               jsonMode, serveResult);
            return 17;
        }
        if (decision == 's') {
            backupKept = true;
        }

        if (!backupKept) {
            // Free-space pre-flight is warn-only; real copy errors still abort.
            CacheEnumerateResult en = enumerateTree(cacheDir);
            if (en.ok) {
                uint64_t avail = 0;
                if (!tsdev::queryVolumeFreeSpace(backupRoot, avail)) {
                    std::fprintf(stderr,
                        "tsdev_cli: WARNING — could not determine free "
                        "space on the backup volume\n");
                } else if (avail < en.totalBytes) {
                    std::fprintf(stderr,
                        "tsdev_cli: WARNING — low disk space on the backup "
                        "volume (needed=%llu bytes, available=%llu bytes); "
                        "the backup may fail\n",
                        static_cast<unsigned long long>(en.totalBytes),
                        static_cast<unsigned long long>(avail));
                }
            }
            CacheProgressRenderer renderer("BACKUP", jsonMode);
            backupRes = backupCache(
                cacheDir, backupRoot,
                [&renderer](uint64_t d, uint64_t t, const std::wstring& f) {
                    renderer.progress(d, t, f);
                });
            renderer.finish();
            if (backupRes.status != CacheOpStatus::Ok) {
                const std::string msg =
                    "cache backup failed: " +
                    wideToUtf8Cli(backupRes.errorPath) + " " +
                    backupRes.message + " (err=" +
                    std::to_string(
                        static_cast<unsigned long>(backupRes.win32Error)) +
                    ")";
                std::fprintf(stderr, "tsdev_cli: ERROR — %s\n",
                             msg.c_str());
                std::fprintf(stderr,
                    "tsdev_cli: cache backup failed — %s\n",
                    tail.c_str());
                emitCacheErrorJson(cacheOp, 16, msg, jsonMode, serveResult);
                return 16;
            }
            std::fprintf(stderr,
                "BACKUP COMPLETE path=%s files=%llu bytes=%llu "
                "elapsed_secs=%llu%s\n",
                wideToUtf8Cli(backupRoot + L"\\cache").c_str(),
                static_cast<unsigned long long>(backupRes.files),
                static_cast<unsigned long long>(backupRes.bytes),
                static_cast<unsigned long long>(backupRes.elapsedSecs),
                liveSourceSuffix(backupRes));
        }

        // A complete backup now exists in the root (freshly staged OR kept
        // existing). Kick detached low-priority NTFS in-place compression of
        // backupRoot\cache — opportunistic; the insurance is already secured,
        // and a partially compressed tree is always valid/readable. Restore
        // output is never compressed (it is the live user cache).
        if (!noCacheCompress) {
            std::fprintf(stderr,
                "tsdev_cli: compressing cache backup in background (NTFS)\n");
            tsdev::startBackupCompression(backupRoot + L"\\cache");
        }
    }

    CacheOpResult restoreRes{};
    if (doRestore) {
        CacheProgressRenderer renderer("RESTORE", jsonMode);
        restoreRes = restoreCache(
            backupRoot, cacheDir,
            [&renderer](uint64_t d, uint64_t t, const std::wstring& f) {
                renderer.progress(d, t, f);
            });
        // Suppress the cosmetic final pct=0 frame when the restore never
        // started copying (NotFound/InUse before any bytes moved).
        if (restoreRes.status == CacheOpStatus::Ok ||
            restoreRes.files != 0 || restoreRes.bytes != 0) {
            renderer.finish();
        }
        if (restoreRes.status == CacheOpStatus::NotFound) {
            // Append the engine's reason when set (e.g. "unsupported backup
            // format 2" or "no complete backup found") — the root path stays.
            std::string msg =
                "no cache backup found at " + wideToUtf8Cli(backupRoot);
            if (!restoreRes.message.empty()) {
                msg += ": " + restoreRes.message;
            }
            std::fprintf(stderr, "tsdev_cli: ERROR — %s\n", msg.c_str());
            emitCacheErrorJson(cacheOp, 5, msg, jsonMode, serveResult);
            return 5;
        }
        if (restoreRes.status == CacheOpStatus::InUse) {
            const std::string msg =
                "cannot restore cache: " +
                wideToUtf8Cli(restoreRes.errorPath) + " is locked. "
                "Close TradeStation Desktop (orchart.exe, WHServer.exe, "
                "TSDev.exe) and retry.";
            std::fprintf(stderr, "tsdev_cli: ERROR — %s\n", msg.c_str());
            emitCacheErrorJson(cacheOp, 16, msg, jsonMode, serveResult);
            return 16;
        }
        if (restoreRes.status != CacheOpStatus::Ok) {
            const std::string msg =
                "cache restore failed: " +
                wideToUtf8Cli(restoreRes.errorPath) + " " +
                restoreRes.message + " (err=" +
                std::to_string(
                    static_cast<unsigned long>(restoreRes.win32Error)) +
                ")";
            std::fprintf(stderr, "tsdev_cli: ERROR — %s\n", msg.c_str());
            std::fprintf(stderr,
                "tsdev_cli: cache restore failed — %s\n",
                tail.c_str());
            emitCacheErrorJson(cacheOp, 16, msg, jsonMode, serveResult);
            return 16;
        }
        std::fprintf(stderr,
            "RESTORE COMPLETE path=%s files=%llu bytes=%llu "
            "elapsed_secs=%llu\n",
            wideToUtf8Cli(cacheDir).c_str(),
            static_cast<unsigned long long>(restoreRes.files),
            static_cast<unsigned long long>(restoreRes.bytes),
            static_cast<unsigned long long>(restoreRes.elapsedSecs));
        std::fprintf(stderr,
            "tsdev_cli: cache restored — TradeStation Desktop will "
            "revalidate it on next start.\n");
    }

    if (!subcommand.empty()) {
        return -1;  // continue to subcommand dispatch (cache phase is done)
    }

    // Serve-request path: hand the exact standalone --json payload back in
    // the CliResult (cliResultToJson merges extraPayload into the response
    // frame — op/success/exit_code are identical to the CliResult's own).
    // Nothing is written to stdout: the broker wire is the pipe frame.
    if (serveResult) {
        serveResult->op           = cacheOp;
        serveResult->success      = true;
        serveResult->exitCode     = 0;
        serveResult->extraPayload = cacheSuccessJson(
            cacheOp, doBackup, doRestore, backupKept, backupRes, restoreRes,
            backupRoot, cacheDir);
        return 0;
    }

    // Standalone invocation: the cache phase owns the stdout result.
    if (jsonMode) {
        // One stdout object for the whole standalone run so json.loads on
        // all-of-stdout works even when both flags were given.
        const ordered_json j = cacheSuccessJson(
            cacheOp, doBackup, doRestore, backupKept, backupRes, restoreRes,
            backupRoot, cacheDir);
        const std::string s =
            j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        std::fwrite(s.c_str(), 1, s.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    } else {
        if (doBackup) {
            if (backupKept) {
                std::fprintf(stdout, "BACKUP KEPT path=%s created=%s\n",
                             wideToUtf8Cli(backupRoot + L"\\cache").c_str(),
                             readBackupInfo(backupRoot).created.c_str());
            } else {
                std::fprintf(stdout,
                    "BACKUP COMPLETE path=%s files=%llu bytes=%llu "
                    "elapsed_secs=%llu%s\n",
                    wideToUtf8Cli(backupRoot + L"\\cache").c_str(),
                    static_cast<unsigned long long>(backupRes.files),
                    static_cast<unsigned long long>(backupRes.bytes),
                    static_cast<unsigned long long>(backupRes.elapsedSecs),
                    liveSourceSuffix(backupRes));
            }
        }
        if (doRestore) {
            std::fprintf(stdout,
                "RESTORE COMPLETE path=%s files=%llu bytes=%llu "
                "elapsed_secs=%llu\n",
                wideToUtf8Cli(cacheDir).c_str(),
                static_cast<unsigned long long>(restoreRes.files),
                static_cast<unsigned long long>(restoreRes.bytes),
                static_cast<unsigned long long>(restoreRes.elapsedSecs));
        }
        std::fflush(stdout);
    }
    return 0;
}

// --- TVS-D.3: reusable per-op logic (shared by single-item + batch) --------
//
// doInsert/doModify/doDelete contain the core operation logic operating on an
// ALREADY-CONSTRUCTED TsDevLib instance. They do NOT construct or destruct the
// library (that is the caller's responsibility — single-item constructs per
// call, batch constructs once and reuses). This avoids duplicating the
// createEntry/saveSource/verifyModule/deleteEntry sequence.
//
// --- TVS-L.9: shared post-verify persistence-integrity check ---------------
//
// Fault A (docs/RE/RE_L8_STALE_WRITE_FAULT_A.md): when the .ell write fails at
// the OS level but ELMDCore.dll forces `true`, the operation reports
// exit 0 / success=true while the .ell is left STALE (hash unchanged) and the
// .xml is rewritten with a NEW GUID that has ZERO occurrences in the stale
// .ell. The existing .ell/.xml existence checks do NOT catch this (both files
// exist). The shared validator in src/persistence_integrity.cpp classifies the
// cross-store consistency; the helpers below snapshot pre-op hashes and apply
// ONE shared post-verify check that both doInsert and doModify call AFTER
// orlib reports success and the existing existence checks pass. Upsert
// inherits the check via doInsert/doModify (it delegates to one of them).
//
// Per the approved L.8 decision gate (BLOCKED recovery): this is DETECT-AND-
// FAIL only. It does NOT restore the .ell/.xml, does NOT invent a
// rollback/unverify/retry API, and does NOT delete the warehouse component.
// On any non-Verified/non-NoOp verdict it sets success=false, exit 9 (the
// existing "artifact_missing" code, reused by all persistence-failure paths),
// and a persistence-specific structured reason in errorMessage/humanStderr
// that includes type / ProgId / .ell path / .xml path.

// sha256FileCli: compute the uppercase-hex SHA-256 of a regular file at
// `path` via the native Win32 bcrypt provider. The status is deliberately
// separate from the hash: an absent path is the only condition that may be
// converted to the no-prior-artifact sentinel by doModify(). An existing path
// that is a directory, cannot be opened/read, or cannot be hashed is an Error
// and must fail closed. Mirrors the validator's internal hasher; kept local to
// this TU because persistence_integrity.h exposes only
// validatePersistenceIntegrity() (matched exactly — no extra declarations
// added).
enum class Sha256FileStatus {
    Hashed,
    Absent,
    Error,
};

struct Sha256FileResult {
    Sha256FileStatus status;
    std::string hash;
};

Sha256FileResult sha256FileCli(const std::string& path) {
    // GetFileAttributesA distinguishes a genuinely absent artifact from an
    // existing artifact that the hash operation cannot evaluate. In
    // particular, do not turn ERROR_ACCESS_DENIED (or any other non-absence
    // error) into the no-prior-artifact sentinel.
    DWORD attributes = GetFileAttributesA(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return {Sha256FileStatus::Absent, std::string()};
        }
        return {Sha256FileStatus::Error, std::string()};
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return {Sha256FileStatus::Error, std::string()};
    }

    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        // The path existed at the classification point. A race that removes
        // it before this open is treated as an error rather than guessed to be
        // a missing pre-op artifact; fail closed for the modify operation.
        return {Sha256FileStatus::Error, std::string()};
    }
    struct FileGuard {
        HANDLE h;
        ~FileGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    } fileGuard{hFile};

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                                  nullptr, 0);
    if (status != 0) return {Sha256FileStatus::Error, std::string()};
    struct AlgGuard {
        BCRYPT_ALG_HANDLE h;
        ~AlgGuard() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
    } algGuard{hAlg};

    DWORD hashObjLen = 0, cbResult = 0;
    status = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&hashObjLen),
                               sizeof(DWORD), &cbResult, 0);
    if (status != 0 || hashObjLen == 0) {
        return {Sha256FileStatus::Error, std::string()};
    }
    DWORD hashLen = 0;
    status = BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                               reinterpret_cast<PUCHAR>(&hashLen),
                               sizeof(DWORD), &cbResult, 0);
    if (status != 0 || hashLen == 0) {
        return {Sha256FileStatus::Error, std::string()};
    }

    std::vector<uint8_t> hashObj(hashObjLen);
    BCRYPT_HASH_HANDLE hHash = nullptr;
    status = BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjLen,
                              nullptr, 0, 0);
    if (status != 0) return {Sha256FileStatus::Error, std::string()};
    struct HashGuard {
        BCRYPT_HASH_HANDLE h;
        ~HashGuard() { if (h) BCryptDestroyHash(h); }
    } hashGuard{hHash};

    const DWORD chunkSize = 65536;
    std::vector<uint8_t> chunk(chunkSize);
    for (;;) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hFile, chunk.data(), chunkSize, &bytesRead, nullptr);
        if (!ok) return {Sha256FileStatus::Error, std::string()};
        if (bytesRead == 0) break;  // EOF
        status = BCryptHashData(hHash, chunk.data(), bytesRead, 0);
        if (status != 0) return {Sha256FileStatus::Error, std::string()};
    }

    std::vector<uint8_t> hash(hashLen);
    status = BCryptFinishHash(hHash, hash.data(), hashLen, 0);
    if (status != 0) return {Sha256FileStatus::Error, std::string()};

    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(hash.size() * 2);
    for (uint8_t b : hash) {
        out.push_back(hex[(b >> 4) & 0xF]);
        out.push_back(hex[b & 0xF]);
    }
    if (out.empty()) return {Sha256FileStatus::Error, std::string()};
    return {Sha256FileStatus::Hashed, out};
}

// emptyBufferSha256: the SHA-256 of a zero-byte buffer, computed once and
// cached. Used as the "pre-hash" sentinel when a pre-op artifact did NOT
// exist (insert of a brand-new item, or modify of a corrupt item whose
// .ell/.xml were missing before the operation). It is a real, non-empty hex
// SHA-256, so the validator does NOT treat it as "unknown" (the header maps
// only an EMPTY pre-hash to InvalidInput). Because a valid post-op .ell/.xml
// is never zero bytes, this sentinel correctly forces ellChanged/xmlChanged =
// true for any real post-op artifact — i.e. "there was no prior file, so the
// post-op file is by definition a change". A zero-byte (corrupt) post-op file
// would hash-equal the sentinel -> ellChanged=false -> FaultA_StaleEll, which
// is the correct detect-and-fail outcome for a corrupt artifact.
//
// Computed directly via bcrypt (hash of zero bytes — no BCryptHashData call).
// This is the well-known constant E3B0C442...B855; computing it avoids
// hardcoding a magic constant and stays consistent with the project's
// bcrypt-everywhere style.
const std::string& emptyBufferSha256() {
    static std::string cached;
    static bool initialized = false;
    if (initialized) return cached;
    initialized = true;

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                                  nullptr, 0);
    if (status != 0) { cached.clear(); return cached; }
    struct AlgGuard {
        BCRYPT_ALG_HANDLE h;
        ~AlgGuard() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
    } algGuard{hAlg};

    DWORD hashObjLen = 0, cbResult = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                      reinterpret_cast<PUCHAR>(&hashObjLen),
                      sizeof(DWORD), &cbResult, 0);
    DWORD hashLen = 0;
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                      reinterpret_cast<PUCHAR>(&hashLen),
                      sizeof(DWORD), &cbResult, 0);
    std::vector<uint8_t> hashObj(hashObjLen);
    BCRYPT_HASH_HANDLE hHash = nullptr;
    BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjLen, nullptr, 0, 0);
    struct HashGuard {
        BCRYPT_HASH_HANDLE h;
        ~HashGuard() { if (h) BCryptDestroyHash(h); }
    } hashGuard{hHash};
    // Hash ZERO bytes (no BCryptHashData call).
    std::vector<uint8_t> hash(hashLen);
    BCryptFinishHash(hHash, hash.data(), hashLen, 0);

    static const char* hex = "0123456789ABCDEF";
    cached.reserve(hash.size() * 2);
    for (uint8_t b : hash) {
        cached.push_back(hex[(b >> 4) & 0xF]);
        cached.push_back(hex[b & 0xF]);
    }
    return cached;
}

// noPriorArtifactHash: the pre-hash to pass for an artifact that did NOT
// exist before the operation. Returns the empty-buffer SHA-256 (see
// emptyBufferSha256). Falls back to a clearly-non-matching sentinel only if
// bcrypt itself fails to produce the empty-buffer hash (extremely unlikely;
// the validator would then still see a non-empty pre-hash that differs from
// any real post-op hash -> ellChanged=true).
const std::string& noPriorArtifactHash() {
    const std::string& h = emptyBufferSha256();
    if (!h.empty()) return h;
    static const std::string fallback = "<no-prior-artifact>";
    return fallback;
}

// verdictLabel: human-readable label for an IntegrityVerdict, used in the
// persistence-specific structured reason message.
const char* verdictLabel(IntegrityVerdict v) {
    switch (v) {
        case IntegrityVerdict::VerifiedRefresh:  return "VerifiedRefresh";
        case IntegrityVerdict::NoOpAllowed:      return "NoOpAllowed";
        case IntegrityVerdict::FaultA_StaleEll:  return "FaultA_StaleEll";
        case IntegrityVerdict::InvalidInput:     return "InvalidInput";
    }
    return "Unknown";
}

// applyPersistenceIntegrityCheck: the SINGLE shared post-verify check called
// by doInsert and doModify AFTER orlib reports success and the existing
// .ell/.xml existence checks pass. Classifies the cross-store consistency via
// validatePersistenceIntegrity().
//
// Inputs:
//   r                 the in-progress CliResult (op/typeStr/name/progId already
//                     set by the caller). On failure this is mutated to
//                     success=false / exit 9 with the persistence-specific
//                     reason. On success it is left untouched (the caller
//                     fills in the success fields).
//   ellPath, xmlPath  the POST-op artifact paths (regular files at this point
//                     — the existence checks already passed).
//   preEllSha256,
//   preXmlSha256      SHA-256 of the PRE-op artifacts, or noPriorArtifactHash()
//                     when the pre-op artifact did not exist. NEVER empty
//                     (the validator returns InvalidInput for an empty
//                     pre-hash, which this check would then report as a
//                     persistence failure).
//   sourceEqualProven true iff the caller proved the new source is
//                     byte-identical to the stored source via getSource()
//                     BEFORE the operation. Only then may an unchanged .ell
//                     hash be accepted as a no-op (NoOpAllowed) rather than
//                     rejected as a stale write (FaultA_StaleEll).
//   noOpRefresh       [out] set to true when the verdict is NoOpAllowed, so
//                     the caller can report success honestly (NOT as a
//                     verified refresh — the .ell hash is expected unchanged
//                     because the source was byte-identical).
//
// Returns true if the check passes (VerifiedRefresh or NoOpAllowed); returns
// false and fills r with exit 9 + a persistence-specific structured reason if
// the check fails (FaultA_StaleEll or InvalidInput).
bool applyPersistenceIntegrityCheck(CliResult& r,
                                    const std::string& ellPath,
                                    const std::string& xmlPath,
                                    const std::string& preEllSha256,
                                    const std::string& preXmlSha256,
                                    bool sourceEqualProven,
                                    bool& noOpRefresh) {
    noOpRefresh = false;

    IntegrityInput in;
    in.ellPath = ellPath;
    in.xmlPath = xmlPath;
    in.preEllSha256 = preEllSha256;
    in.preXmlSha256 = preXmlSha256;
    in.sourceEqualProven = sourceEqualProven;

    IntegrityVerdict v = validatePersistenceIntegrity(in);

    if (v == IntegrityVerdict::VerifiedRefresh) {
        return true;  // real, runtime-linkable refresh — caller reports success
    }
    if (v == IntegrityVerdict::NoOpAllowed) {
        noOpRefresh = true;  // genuine no-op — caller reports success (NOT a refresh)
        return true;
    }

    // FaultA_StaleEll or InvalidInput: detect-and-fail. Reuse exit 9
    // ("artifact_missing", the existing code for .ell/.xml integrity failures,
    // reused by ALL persistence-failure paths per the L.9 constraint). The
    // persistence-specific structured reason (verdict + type/ProgId/paths) goes
    // into errorMessage (-> JSON error.message) and humanStderr.
    r.success = false;
    r.exitCode = 9;
    r.ellPath = ellPath;  // keep the path for JSON ell_path on this failure

    const char* reason = (v == IntegrityVerdict::InvalidInput)
        ? "persistence integrity check could not evaluate the artifacts "
          "(unreadable/unparseable .ell or .xml, or GUID missing from .xml)"
        : "persistence integrity check FAILED — stale .ell (Fault A): the "
          ".ell hash is unchanged or the post-op XML GUID is not present "
          "exactly once (mixed-endian) in the .ell; the artifacts are "
          "inconsistent and the item is runtime-unlinkable";

    r.errorMessage = std::string(reason)
        + " [verdict=" + verdictLabel(v)
        + ", type=" + r.typeStr
        + ", name=\"" + r.name + "\""
        + ", ProgId=" + std::to_string(r.progId)
        + ", .ell=" + ellPath
        + ", .xml=" + xmlPath + "]";

    r.humanStderr = std::string("tsdev_cli: persistence integrity FAILED for ")
        + r.typeStr + " \"" + r.name + "\" (ProgId=" + std::to_string(r.progId)
        + ") [verdict=" + verdictLabel(v) + "]\n"
        + "  .ell=" + ellPath + "\n"
        + "  .xml=" + xmlPath + "\n"
        + "  " + reason + "\n"
        + "  (detect-and-fail; no rollback/retry attempted)\n";
    return false;
}

// --- TVS-L.10: SINGLE create-path partial-entry cleanup owner ---------------
//
// cleanupCreatePathFailure is the ONE place that decides whether a partial
// warehouse entry created by a successful createEntry is removed when the
// create-path subsequently fails, and performs the removal. It is called by
// doInsert on EVERY post-createEntry failure branch (saveSource fail AND
// every verifyModule failure mode). Upsert-new inherits it via doInsert.
//
// Contract:
//   * `progId` is the ProgId returned by the successful createEntry (> 0).
//   * `type` / `name` are the EXACT warehouse/API name + ComponentType passed
//     to createEntry (no EL source parsing — HARD RULE 0). deleteEntry uses
//     them to locate the partial entry via FindExistPos and the .ell/.xml
//     paths via the expanded whType (the enum value).
//   * `stage` identifies WHICH post-create step failed (SaveSourceFailed or
//     one of the VerifyModule_* enumerators). It is passed to
//     requiresCreatePathCleanup() so the ownership decision is routed through
//     the single pure function pinned by the unit test, and so the stage is
//     visible in diagnostics.
//
// Cleanup action: deleteEntry(type, name, flush=false). This removes the
// partial warehouse entry (FindExistPos + FUN_101a8360 delete orchestrator +
// InitGlobalLookup/DeleteFromTree BST update) AND deletes both artifacts
// (.ell and .xml) from disk via deleteEntry's existing step 5. flush=false
// is used in BOTH single-item and batch/serve paths: in batch/serve the
// warehouse must stay open for the session lifetime (closing it frees the
// IWHouse interface at [warehouse+0x5E] and crashes subsequent ops), and in
// the single-item path the destructor's OrlibExit persists the deletion
// anyway. This matches the existing compile-fail cleanup behavior.
//
// Diagnostics: the PRIMARY failure (exit 7/8) is already recorded in `r`
// by the caller. Cleanup errors are attached SECONDARY:
//   * On successful cleanup: a one-line notice is appended to humanStderr
//     (informational, not an error). cleanupError is left empty so JSON
//     emits no cleanup_error field.
//   * On cleanup FAILURE: the failure reason is appended to humanStderr AND
//     recorded in r.cleanupError (emitted as error.cleanup_error in JSON).
//     The primary exitCode / errorMessage / compileErrors are untouched, so
//     a cleanup failure can never look like full success (the result stays
//     exit 7/8, success=false). The caller's return is unaffected — it
//     still returns the original failure result.
//
// Returns true if cleanup succeeded (or was not required), false if the
// cleanup itself failed. The return value is informational; the caller does
// NOT change its exit code based on it.
bool cleanupCreatePathFailure(TsDevLib& lib, ComponentType type,
                              const std::string& typeStr,
                              const std::string& name, int progId,
                              CreatePathFailureStage stage, CliResult& r) {
    if (!tsdev::requiresCreatePathCleanup(progId, stage)) {
        // createEntry did not succeed (progId <= 0) — no partial entry exists.
        // Nothing to clean up. (Reached only defensively; doInsert never
        // calls this when progId <= 0.)
        return true;
    }

    const char* stageLabel = "unknown";
    switch (stage) {
        case CreatePathFailureStage::SaveSourceFailed:           stageLabel = "saveSource"; break;
        case CreatePathFailureStage::VerifyModule_Oom:           stageLabel = "verifyModule-OOM"; break;
        case CreatePathFailureStage::VerifyModule_SehCtor:       stageLabel = "verifyModule-SEH(ctor)"; break;
        case CreatePathFailureStage::VerifyModule_SehStartCompiling:
            stageLabel = "verifyModule-SEH(StartCompiling)"; break;
        case CreatePathFailureStage::VerifyModule_SehVerify:     stageLabel = "verifyModule-SEH(Verify)"; break;
        case CreatePathFailureStage::VerifyModule_CompileErrors: stageLabel = "verifyModule-compileErrors"; break;
        // TVS-L.11: post-verify failure branches (after successful create+verify).
        case CreatePathFailureStage::PostVerify_EllMissing:           stageLabel = "postVerify-ellMissing"; break;
        case CreatePathFailureStage::PostVerify_XmlMissing:           stageLabel = "postVerify-xmlMissing"; break;
        case CreatePathFailureStage::PostVerify_PersistenceIntegrity: stageLabel = "postVerify-persistenceIntegrity"; break;
        // TVS-L.11: defensive default — every CreatePathFailureStage enumerator
        // is handled above, but a future addition must not silently fall
        // through to the "unknown" initial value without surfacing here.
        default:
            stageLabel = "unknown";
            break;
    }

    std::string note = std::string("tsdev_cli: create-path failed at ")
        + stageLabel + " after successful createEntry — cleaning up partial "
        + typeStr + " \"" + name + "\" (ProgId=" + std::to_string(progId)
        + ")\n";

    bool cleanupOk = false;
    std::string cleanupErr;
    // TVS-L.12: deleteEntry now classifies each DeleteFileA failure via
    // deleteDiskCleanupErrorFailsResult() and, when a real removal failure
    // occurs (e.g. ERROR_ACCESS_DENIED on a read-only .ell), returns false
    // AND populates diskCleanupErrorOut with the residual path + Windows
    // error. Capture that detail so it can be folded into cleanupErr below
    // and surfaced in r.cleanupError (JSON error.cleanup_error) — a stale
    // .ell/.xml left behind by the cleanup delete must never look like a
    // clean cleanup. This does NOT invent attrib -R or any recovery; it only
    // reports the residual the orchestrator already surfaced.
    std::string diskCleanupError;
    try {
        // flush=false: keeps the warehouse open for batch/serve lifetime.
        // deleteEntry removes the warehouse entry (BST update via
        // DeleteFromTree) AND both artifacts (.ell/.xml) from disk.
        cleanupOk = lib.deleteEntry(type, name, /*flush=*/false,
                                    &diskCleanupError);
        if (!cleanupOk) {
            // deleteEntry returns false for two distinct reasons: an earlier
            // failure (not found / orchestrator SEH — diskCleanupError stays
            // empty) OR a disk-cleanup failure classified by the L.12
            // decision (diskCleanupError carries the residual path + Windows
            // error). Distinguish them so the secondary diagnostic names the
            // residual artifact when one is present.
            if (diskCleanupError.empty()) {
                cleanupErr = "deleteEntry returned false (entry may have "
                             "already been removed or FindExistPos missed "
                             "it)";
            } else {
                cleanupErr = "deleteEntry returned false — disk cleanup "
                             "failed; residual artifact(s): "
                             + diskCleanupError;
            }
        }
    } catch (const std::exception& e) {
        cleanupErr = std::string("deleteEntry threw: ") + e.what();
    } catch (...) {
        cleanupErr = "deleteEntry threw unknown exception";
    }

    if (cleanupOk) {
        note += "tsdev_cli: cleanup succeeded — partial entry + .ell/.xml removed\n";
        r.humanStderr += note;
    } else {
        // Attach the cleanup failure SECONDARY. The primary diagnostic
        // (exitCode / errorMessage / compileErrors) is preserved untouched;
        // success stays false. cleanupError is emitted as error.cleanup_error
        // in JSON so a cleanup failure is always visible and never looks like
        // full success.
        r.cleanupError = std::string("create-path cleanup (stage=")
            + stageLabel + ") failed: " + cleanupErr;
        r.humanStderr += note;
        r.humanStderr += std::string("tsdev_cli: WARNING — create-path cleanup of \"")
            + name + "\" (ProgId=" + std::to_string(progId)
            + ") failed: " + cleanupErr + "\n";
    }
    return cleanupOk;
}

// doInsert: when `flushPerItem` is true (single-item path), closeWarehouse() is
// called AFTER all post-verify validation (.ell/.xml existence + L.9
// persistence-integrity check) succeeds — so that any cleanupCreatePathFailure
// branch can still call deleteEntry() against an open warehouse (TVS-L.11).
// When false (batch path), closeWarehouse is skipped here; the batch worker
// calls it ONCE at the end of the batch.
CliResult doInsert(TsDevLib& lib, ComponentType type,
                   const std::string& typeStr, const std::string& name,
                   const std::string& source, bool flushPerItem) {
    CliResult r;
    r.op = "insert";
    r.typeStr = typeStr;
    r.name = name;

    // 1. Check if the component already exists — fail loudly, no overwrite.
    int existing = lib.findExistPos(name, type);
    // §7.1 diag: "findExistPos" batch-phase marker.
    TSDEV_DIAG_LOG("batch.phase.findExistPos", "item=%s exists=%d",
                   name.c_str(), existing);
    if (existing > 0) {
        r.exitCode = 5;
        r.existingProgId = existing;
        r.errorMessage = typeStr + " \"" + name + "\" already exists (ProgId="
                         + std::to_string(existing) + "). Use modify instead.";
        r.humanStderr = std::string("tsdev_cli: ERROR — ") + typeStr + " \""
                        + name + "\" already exists (ProgId=" + std::to_string(existing)
                        + "). Use modify instead.\n";
        return r;
    }

    // 2. Create the entry.
    int errCode = 0;
    int progId = lib.createEntry(type, name, errCode);
    // §7.1 diag: "createEntry" batch-phase marker.
    TSDEV_DIAG_LOG("batch.phase.createEntry", "item=%s progId=%d errCode=%d",
                   name.c_str(), progId, errCode);
    if (progId <= 0) {
        r.exitCode = 6;
        r.progId = progId;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "0x%X", errCode);
        r.errorMessage = std::string("createEntry failed (ProgId=")
                         + std::to_string(progId) + ", errCode=" + buf + ")";
        r.humanStderr = std::string("tsdev_cli: createEntry failed (ProgId=")
                        + std::to_string(progId) + ", errCode=" + buf + ")\n";
        return r;
    }

    // 3. Save the source.
    const bool savedSource = lib.saveSource(type, progId, source);
    // §7.1 diag: "saveSource" batch-phase marker.
    TSDEV_DIAG_LOG("batch.phase.saveSource", "item=%s progId=%d ok=%d",
                   name.c_str(), progId, savedSource ? 1 : 0);
    if (!savedSource) {
        r.exitCode = 7;
        r.progId = progId;
        r.errorMessage = std::string("saveSource failed for ProgId=")
                         + std::to_string(progId);
        r.humanStderr = std::string("tsdev_cli: saveSource failed for ProgId=")
                        + std::to_string(progId) + "\n";
        // TVS-L.10: createEntry succeeded (progId > 0) but saveSource failed.
        // The partial warehouse entry is orphaned without this — the previous
        // code had NO cleanup on this path. Route through the SINGLE owner.
        // Cleanup errors attach secondarily; the primary exit-7 diagnostic
        // is preserved and success stays false.
        cleanupCreatePathFailure(lib, type, typeStr, name, progId,
                                 CreatePathFailureStage::SaveSourceFailed, r);
        return r;
    }

    // 4. Compile / verify — fail loudly on errors.
    //    TVS-L.10: pass &verifyStage so that if verifyModule fails, the
    //    SINGLE cleanup owner can report WHICH internal path failed (OOM,
    //    SEH-ctor, SEH-StartCompiling, SEH-Verify, or compile errors) and
    //    route the cleanup through requiresCreatePathCleanup(). The stage
    //    is initialized to CompileErrors as a defensive default; verifyModule
    //    overwrites it on every non-empty return path.
    CreatePathFailureStage verifyStage = CreatePathFailureStage::VerifyModule_CompileErrors;
    std::string errors = lib.verifyModule(type, progId, &verifyStage);
    // §7.1 diag: "verify" batch-phase marker (link/traverse sub-phases are
    // logged inside verifyModule as phase.link / phase.traverse).
    TSDEV_DIAG_LOG("batch.phase.verify", "item=%s progId=%d errors=%zu",
                   name.c_str(), progId, errors.size());
    if (!errors.empty()) {
        r.exitCode = 8;
        r.progId = progId;
        r.errorMessage = "compilation FAILED";
        r.compileErrors = errors;
        r.humanStderr = std::string("tsdev_cli: compilation FAILED for \"")
                        + name + "\" (ProgId=" + std::to_string(progId)
                        + "):\n" + errors + "\n";
        // TVS-L.10: createEntry succeeded (progId > 0) but verify/compile
        // failed (OOM / SEH-ctor / SEH-StartCompiling / SEH-Verify / compile
        // errors). The previous code cleaned up ONLY the compile-error case
        // (and only incidentally the SEH/OOM cases via the !errors.empty()
        // branch inside verifyModule). Route ALL of them through the SINGLE
        // owner. Cleanup errors attach secondarily; the primary exit-8
        // diagnostic + compileErrors are preserved and success stays false.
        cleanupCreatePathFailure(lib, type, typeStr, name, progId,
                                 verifyStage, r);
        return r;
    }

    // 5. Verify the .ell file exists.
    //    TVS-L.11: the post-verify existence checks (.ell/.xml), the L.9
    //    persistence-integrity check, and any L.10 cleanupCreatePathFailure
    //    MUST run BEFORE closeWarehouse in the single-item path. Closing the
    //    warehouse first would free the orlib20 warehouse handle, so the
    //    deleteEntry() inside cleanupCreatePathFailure could no longer operate
    //    (FindExistPos / FUN_101a8360 / BST update all need an open warehouse).
    //    closeWarehouse is therefore deferred to step 8, AFTER successful
    //    validation. In the batch/serve path (flushPerItem == false) the
    //    warehouse is never closed here — the batch worker closes it once at
    //    the end of the session, preserving the session lifetime.
    std::string ellPath = lib.getEllPath(type, progId);
    if (!lib.ellFileExists(type, progId)) {
        r.exitCode = 9;
        r.progId = progId;
        r.errorMessage = std::string("compilation succeeded but .ell file not found: ")
                         + ellPath;
        r.humanStderr = std::string("tsdev_cli: compilation succeeded but .ell file not found: ")
                        + ellPath + "\n";
        // TVS-L.11: createEntry + verify succeeded (progId > 0) but the .ell
        // artifact is missing on disk — the partial warehouse entry is
        // orphaned without this. Route through the SAME L.10 single owner.
        // Cleanup errors attach secondarily; the primary exit-9 diagnostic
        // is preserved and success stays false.
        cleanupCreatePathFailure(lib, type, typeStr, name, progId,
                                 CreatePathFailureStage::PostVerify_EllMissing, r);
        return r;
    }

    // 6. Verify the .xml file exists.
    if (!lib.xmlFileExists(type, progId)) {
        r.exitCode = 9;
        r.progId = progId;
        r.errorMessage = "compilation succeeded but .xml file not found at "
                        + lib.getXmlPath(type, progId)
                        + " — finalization may be incomplete";
        r.humanStderr = std::string("tsdev_cli: compilation succeeded but .xml file not found at ")
                        + lib.getXmlPath(type, progId)
                        + " — finalization may be incomplete\n";
        // TVS-L.11: createEntry + verify succeeded (progId > 0) but the .xml
        // artifact is missing on disk — route through the SAME L.10 single
        // owner. Cleanup errors attach secondarily; the primary exit-9
        // diagnostic is preserved and success stays false.
        cleanupCreatePathFailure(lib, type, typeStr, name, progId,
                                 CreatePathFailureStage::PostVerify_XmlMissing, r);
        return r;
    }

    // 7. TVS-L.9: shared post-verify persistence-integrity check.
    //    For insert there is NO prior .ell/.xml (it is a brand-new item), so
    //    the pre-hashes are the no-prior-artifact sentinel (forces
    //    ellChanged/xmlChanged = true for any real post-op artifact). The
    //    source is new by construction, so sourceEqualProven = false. The
    //    validator then requires: regular files (already checked above), a
    //    parseable XML GUID, and EXACTLY ONE mixed-endian occurrence of that
    //    GUID in the new .ell — the Fault A signature (new XML GUID with zero
    //    occurrences in a stale/missing .ell) is detected here even on insert.
    {
        std::string xmlPath = lib.getXmlPath(type, progId);
        r.progId = progId;
        bool noOpRefresh = false;
        if (!applyPersistenceIntegrityCheck(r, ellPath, xmlPath,
                                            noPriorArtifactHash(),
                                            noPriorArtifactHash(),
                                            /*sourceEqualProven=*/false,
                                            noOpRefresh)) {
            // TVS-L.11: createEntry + verify succeeded (progId > 0) but the
            // L.9 persistence-integrity validator detected Fault A (stale .ell)
            // or InvalidInput — the artifacts are inconsistent and the item is
            // runtime-unlinkable. Route through the SAME L.10 single owner so
            // the partial warehouse entry + .ell/.xml are removed. Cleanup
            // errors attach secondarily; the primary exit-9 persistence-specific
            // diagnostic (errorMessage / humanStderr) is preserved and success
            // stays false.
            cleanupCreatePathFailure(lib, type, typeStr, name, progId,
                                     CreatePathFailureStage::PostVerify_PersistenceIntegrity, r);
            return r;  // exit 9, persistence-specific reason already in r
        }
        // Insert can never be a no-op (brand-new item); noOpRefresh is ignored.
    }

    // 8. Close the warehouse (flush) — single-item only; batch flushes once
    //    at the end. TVS-L.11: this is deferred until AFTER all post-verify
    //    validation (steps 5–7) succeeds, so that any cleanupCreatePathFailure
    //    branch above could still call deleteEntry() against an open warehouse.
    //    In the batch/serve path (flushPerItem == false) the warehouse is left
    //    open for the session lifetime. A failed explicit close is an operation
    //    failure; do not report the insert as successful.
    if (flushPerItem && !lib.closeWarehouse()) {
        r.exitCode = 6;
        r.progId = progId;
        r.errorMessage = std::string("closeWarehouse failed for ProgId=")
                         + std::to_string(progId);
        r.humanStderr = std::string("tsdev_cli: closeWarehouse failed for ProgId=")
                        + std::to_string(progId) + "\n";
        // §5 close_failed class: the SEH-failed close leaves the session
        // possibly open with uncertain teardown state — the worker must
        // emit + TerminateProcess, never `delete lib`/ExitProcess into it.
        r.sessionMayBeOpen = true;
        return r;
    }

    // 9. Success.
    r.success = true;
    r.exitCode = 0;
    r.progId = progId;
    r.ellPath = ellPath;
    r.humanStdout = std::string("SUCCESS: ") + typeStr + " \"" + name
                    + "\" inserted, ProgId=" + std::to_string(progId)
                    + ", .ell=" + ellPath + "\n";
    return r;
}

// doModify: reuses the library's modifyEntry (findExistPos -> saveSource ->
// verifyModule on the existing ProgId). The ProgId is preserved (no
// createEntry). Does not call closeWarehouse (the original single-item path
// did not either — persistence happens via OrlibExit in the destructor, or
// via the batch worker's single closeWarehouse at the end).
//
// TVS-L.9: BEFORE modifyEntry runs, this function snapshots the pre-op
// .ell/.xml SHA-256 hashes (when the files already exist) and proves or
// disproves source-equality via getSource() — the explicit get-source
// equality check that, when true, lets the shared post-verify validator
// accept an unchanged .ell hash as a genuine no-op (NoOpAllowed) instead of
// rejecting it as a stale write (FaultA_StaleEll). AFTER orlib reports
// success and the existing .ell/.xml existence checks pass, the SINGLE shared
// applyPersistenceIntegrityCheck() classifies the cross-store consistency.
// No EasyLanguage source is parsed for metadata (HARD RULE 0); the only
// metadata read is the GUID from the .xml descriptor artifact.
CliResult doModify(TsDevLib& lib, ComponentType type,
                   const std::string& typeStr, const std::string& name,
                   const std::string& source) {
    CliResult r;
    r.op = "modify";
    r.typeStr = typeStr;
    r.name = name;

    // --- TVS-L.9 pre-op snapshot (when the files already exist) ---
    // Look up the existing ProgId so we can read the pre-op artifacts BEFORE
    // modifyEntry rewrites them. modifyEntry does its own findExistPos
    // internally; this extra lookup is a cheap BST search and keeps the
    // snapshot point unambiguous (before any mutation).
    int preProgId = lib.findExistPos(name, type);
    // §7.1 diag: "findExistPos" batch-phase marker (modify path).
    TSDEV_DIAG_LOG("batch.phase.findExistPos", "item=%s progId=%d",
                   name.c_str(), preProgId);

    std::string preEllPath, preXmlPath;
    std::string preEllHash, preXmlHash;
    bool sourceEqualProven = false;

    if (preProgId > 0) {
        preEllPath = lib.getEllPath(type, preProgId);
        preXmlPath = lib.getXmlPath(type, preProgId);

        // Snapshot pre-op hashes. sha256FileCli() classifies the result into
        // three states so that a genuinely absent artifact (Hashed::Absent) is
        // distinguished from an existing-but-unreadable one (Hashed::Error):
        //   - Hashed:  use the computed hash.
        //   - Absent:  use the no-prior-artifact sentinel (normal absent-file
        //              semantics; the validator forces ellChanged/xmlChanged =
        //              true for the post-op artifacts).
        //   - Error:   the artifact EXISTS but cannot be opened/read/hashed.
        //              Fail closed with exit 9 (artifact_missing, the existing
        //              persistence-integrity error code) BEFORE modifyEntry
        //              mutates anything — do NOT treat the unreadable file as a
        //              new/absent artifact (the original bug: empty string was
        //              silently converted to the no-prior-artifact sentinel).
        auto snapshotPreHash = [&](const std::string& artifactPath,
                                   bool fileExists,
                                   const char* artifactLabel,
                                   std::string& outHash) -> bool {
            if (!fileExists) {
                outHash = noPriorArtifactHash();
                return true;
            }
            Sha256FileResult h = sha256FileCli(artifactPath);
            switch (h.status) {
                case Sha256FileStatus::Hashed:
                    outHash = h.hash;
                    return true;
                case Sha256FileStatus::Absent:
                    // Race: the library reported existence but the file is now
                    // gone. Treat as absent (normal absent-file semantics).
                    outHash = noPriorArtifactHash();
                    return true;
                case Sha256FileStatus::Error:
                    // Existing-but-unreadable: fail closed with exit 9
                    // (artifact_missing), the existing persistence-integrity
                    // error path reused by all persistence-failure paths.
                    r.success = false;
                    r.exitCode = 9;
                    r.progId = preProgId;
                    r.errorMessage = std::string("pre-operation ") + artifactLabel
                        + " artifact exists but cannot be hashed (unreadable"
                        " or hash error) — refusing to treat as a new file"
                        + " [type=" + typeStr
                        + ", name=\"" + name + "\""
                        + ", ProgId=" + std::to_string(preProgId)
                        + ", " + artifactLabel + "=" + artifactPath + "]";
                    r.humanStderr = std::string("tsdev_cli: pre-operation ")
                        + artifactLabel + " artifact exists but cannot be"
                        " hashed at " + artifactPath
                        + " (ProgId=" + std::to_string(preProgId)
                        + ") — aborting modify before mutation"
                        " (detect-and-fail; no rollback/retry attempted)\n";
                    return false;
            }
            return false;  // unreachable
        };

        if (!snapshotPreHash(preEllPath, lib.ellFileExists(type, preProgId),
                             ".ell", preEllHash)) {
            return r;  // exit 9 — pre-op .ell unreadable
        }
        if (!snapshotPreHash(preXmlPath, lib.xmlFileExists(type, preProgId),
                             ".xml", preXmlHash)) {
            return r;  // exit 9 — pre-op .xml unreadable
        }

        // Explicit get-source equality: prove the new source is byte-identical
        // to the STORED source (retrieved via getSource from the warehouse
        // record — NOT parsed from EL source comments). Only when this is
        // true may an unchanged .ell hash be accepted as a no-op. getSource
        // returns empty on failure; in that case sourceEqualProven stays
        // false (fail safe: cannot prove no-op -> require hash refresh).
        std::string storedSource = lib.getSource(type, name);
        if (!storedSource.empty() && storedSource == source) {
            sourceEqualProven = true;
        }
    } else {
        // Item does not exist — let modifyEntry surface the not-found result
        // (exit 5) below. No pre-snapshot is possible.
        preEllHash = noPriorArtifactHash();
        preXmlHash = noPriorArtifactHash();
    }

    Result res = lib.modifyEntry(type, name, source);
    // §7.1 diag: modifyEntry = saveSource + verifyModule (which log their own
    // dispatch/phase records) — mark the enclosing completion.
    TSDEV_DIAG_LOG("batch.phase.modifyEntry", "item=%s success=%d progId=%d",
                   name.c_str(), res.success ? 1 : 0, res.progId);

    if (!res.success) {
        if (res.message == "component not found") {
            r.exitCode = 5;
            r.errorMessage = typeStr + " \"" + name
                             + "\" not found in the warehouse.";
            r.humanStderr = std::string("tsdev_cli: ERROR — ") + typeStr + " \""
                            + name + "\" not found in the warehouse. "
                            "Use insert to create it.\n";
        } else {
            r.exitCode = 8;
            r.progId = res.progId;
            r.errorMessage = "compilation FAILED";
            r.compileErrors = res.message;
            r.humanStderr = std::string("tsdev_cli: modification FAILED for \"")
                            + name + "\" (ProgId=" + std::to_string(res.progId)
                            + "):\n" + res.message + "\n";
        }
        return r;
    }

    std::string ellPath = lib.getEllPath(type, res.progId);
    if (!lib.ellFileExists(type, res.progId)) {
        r.exitCode = 9;
        r.progId = res.progId;
        r.errorMessage = "compilation succeeded but .ell file not found at "
                        + lib.getEllPath(type, res.progId)
                        + " — finalization may be incomplete";
        r.humanStderr = std::string("tsdev_cli: compilation succeeded but .ell file not found at ")
                        + lib.getEllPath(type, res.progId)
                        + " — finalization may be incomplete\n";
        return r;
    }
    if (!lib.xmlFileExists(type, res.progId)) {
        r.exitCode = 9;
        r.progId = res.progId;
        r.errorMessage = "compilation succeeded but .xml file not found at "
                        + lib.getXmlPath(type, res.progId)
                        + " — finalization may be incomplete";
        r.humanStderr = std::string("tsdev_cli: compilation succeeded but .xml file not found at ")
                        + lib.getXmlPath(type, res.progId)
                        + " — finalization may be incomplete\n";
        return r;
    }

    // TVS-L.9: shared post-verify persistence-integrity check. The pre-op
    // hashes were snapshotted above (or the no-prior-artifact sentinel when
    // the pre-op files did not exist). sourceEqualProven is true ONLY when
    // getSource returned byte-identical source before the operation. On
    // Fault A / GUID-missing / stale hash / invalid input: success=false,
    // exit 9, persistence-specific structured reason (verdict + type/ProgId/
    // paths) — detect-and-fail, no rollback/retry (L.8 BLOCKED recovery).
    {
        std::string xmlPath = lib.getXmlPath(type, res.progId);
        r.progId = res.progId;
        bool noOpRefresh = false;
        if (!applyPersistenceIntegrityCheck(r, ellPath, xmlPath,
                                            preEllHash, preXmlHash,
                                            sourceEqualProven,
                                            noOpRefresh)) {
            return r;  // exit 9, persistence-specific reason already in r
        }
        // On NoOpAllowed: report success=true but NOT as a verified refresh.
        // The .ell hash is expected unchanged because the caller proved the
        // source was byte-identical; the human/JSON success message below
        // carries the standard "modified" wording (no "refreshed" claim).
    }

    r.success = true;
    r.exitCode = 0;
    r.progId = res.progId;
    r.ellPath = ellPath;
    r.humanStdout = std::string("SUCCESS: ") + typeStr + " \"" + name
                    + "\" modified, ProgId=" + std::to_string(res.progId)
                    + ", .ell=" + ellPath + "\n";
    return r;
}

// §5 teardown classification for the delete result (close_failed class).
// deleteEntry(flush=true) bundles its closeWarehouse() inside its flow, so
// a false return with an EMPTY diskCleanupError cannot be split further at
// this level — it is one of: the entry vanished before the delete
// (FindExistPos <= 0 inside deleteEntry), the delete orchestrator raised
// SEH, or the close itself failed. In all three the op's close did not
// provably complete, so the session may still be open (the first two leave
// it open and intact — individually `delete lib`-safe — but they cannot be
// separated from the third at the CliResult level without lib changes, so
// all take the fail-closed branch; the observable contract is unchanged).
// A NON-empty diskCleanupError proves the close SUCCEEDED (disk cleanup
// runs only after it) — the session is properly closed. flush=false
// (batch/serve) never attempts the close: the session stays open BY
// DESIGN for the session lifetime and the end-of-session close owns the
// teardown, so the flag stays clear there. Pure decision (same-TU unit
// test pins the full truth table).
bool deleteResultLeavesSessionOpen(bool deleted,
                                   bool diskCleanupErrorEmpty,
                                   bool flushPerItem) {
    return flushPerItem && !deleted && diskCleanupErrorEmpty;
}

// doDelete: findExistPos (distinct not-found message) -> deleteEntry. When
// flushPerItem is true (single-item path), deleteEntry calls closeWarehouse()
// internally as part of its flow. When false (serve/batch path), the
// closeWarehouse is skipped — the warehouse stays open for the session
// lifetime, and runServe/runBatch calls closeWarehouse() once at the end.
// This is critical: closing the warehouse after delete frees the IWHouse
// interface, causing subsequent enumerateRecords/refreshCache calls to crash.
CliResult doDelete(TsDevLib& lib, ComponentType type,
                   const std::string& typeStr, const std::string& name,
                   bool flushPerItem = true) {
    CliResult r;
    r.op = "delete";
    r.typeStr = typeStr;
    r.name = name;

    int progId = lib.findExistPos(name, type);
    if (progId <= 0) {
        r.exitCode = 5;
        r.errorMessage = typeStr + " \"" + name
                         + "\" not found in the warehouse.";
        r.humanStderr = std::string("tsdev_cli: ERROR — ") + typeStr + " \""
                        + name + "\" not found in the warehouse.\n";
        return r;
    }

    // TVS-L.12: deleteEntry reports a disk-cleanup failure (a stale .ell/.xml
    // left on disk) by returning false AND populating `diskCleanupError` with
    // the residual path(s) + Windows error(s). The delete orchestrator has
    // ALREADY run in that case, so the warehouse entry was removed — only the
    // on-disk artifact(s) remain. An explicit closeWarehouse failure also
    // returns false, but occurs before disk cleanup and leaves this diagnostic
    // string empty. Both cases use the existing exit-6 / entry_op_failure
    // result; the populated diagnostic is reserved for the stale-artifact
    // case.
    std::string diskCleanupError;
    bool deleted = lib.deleteEntry(type, name, flushPerItem, &diskCleanupError);
    // §5: did the op's own close provably complete? See the helper comment
    // — true only for a single-op deleteEntry failure with empty
    // diskCleanupError (close failed or never ran => session possibly open).
    r.sessionMayBeOpen = deleteResultLeavesSessionOpen(
        deleted, diskCleanupError.empty(), flushPerItem);
    if (!deleted) {
        r.exitCode = 6;
        r.progId = progId;
        if (!diskCleanupError.empty()) {
            r.errorMessage = std::string("deleteEntry partially failed for ")
                + typeStr + " \"" + name + "\" (ProgId="
                + std::to_string(progId) + "): warehouse entry was removed, "
                "but disk cleanup failed — " + diskCleanupError;
            r.humanStderr = std::string("tsdev_cli: deleteEntry partially "
                "failed for ") + typeStr + " \"" + name + "\" (ProgId="
                + std::to_string(progId) + "): warehouse entry was removed, "
                "but disk cleanup failed — " + diskCleanupError + "\n";
        } else {
            r.errorMessage = std::string("deleteEntry failed for ") + typeStr
                             + " \"" + name + "\" (ProgId="
                             + std::to_string(progId) + ").";
            r.humanStderr = std::string("tsdev_cli: deleteEntry failed for ")
                            + typeStr + " \"" + name + "\" (ProgId="
                            + std::to_string(progId) + ").\n";
        }
        return r;
    }

    r.success = true;
    r.exitCode = 0;
    r.progId = progId;
    r.humanStdout = std::string("SUCCESS: ") + typeStr + " \"" + name
                    + "\" deleted (ProgId=" + std::to_string(progId)
                    + " was removed from warehouse, "
                    ".ell/.xml files removed from disk).\n";
    return r;
}

// doExists: query whether a component exists by name + type. This is a
// QUERY, not a mutation: not-found is a valid result (NOT an error). Both
// found and not-found return exit 0, distinguished by the `exists` field.
//
// When caseInsensitive is true, uses enumerateRecords (case-insensitive
// search) and reports a match regardless of name casing. The verbatim
// stored name is returned via stored_name.
//
// When caseInsensitive is false (default), uses enumerateRecords to find
// the item case-insensitively (since findExistPos is inherently
// case-insensitive), then post-compares the stored name with the query
// name using exact (case-sensitive) comparison. Only reports exists=1
// when the names match exactly.
CliResult doExists(TsDevLib& lib, ComponentType type,
                   const std::string& typeStr, const std::string& name,
                   bool caseInsensitive) {
    CliResult r;
    r.op = "exists";
    r.typeStr = typeStr;
    r.name = name;

    EnumerateResult er = lib.enumerateRecords(type, name);
    if (er.exists) {
        if (caseInsensitive || er.storedName == name) {
            r.success = true;
            r.exitCode = 0;
            r.exists = true;
            r.progId = er.progId;
            r.storedName = er.storedName;
            r.humanStdout = "exists=1\nstored_name=" + er.storedName
                            + "\nprogId=" + std::to_string(er.progId) + "\n";
        } else {
            r.success = true;
            r.exitCode = 0;
            r.exists = false;
            r.progId = 0;
            r.storedName = "";
            r.humanStdout = "exists=0\nstored_name=\nprogId=0\n";
        }
    } else {
        r.success = true;
        r.exitCode = 0;
        r.exists = false;
        r.progId = 0;
        r.storedName = "";
        r.humanStdout = "exists=0\nstored_name=\nprogId=0\n";
    }
    return r;
}

// doUpsert: insert-or-modify by name + type + source. Calls findExistPos
// FIRST, then branches — avoids the overhead of a failed insert attempt.
// When the item exists (ProgId > 0), delegates to doModify (preserves the
// ProgId); otherwise delegates to doInsert with the caller-supplied
// flushPerItem (true for single-item, false for batch) (assigns a
// new ProgId). The op field is overridden to "upsert" after delegation
// (doInsert/doModify set their own op internally). Upsert never emits exit 5
// (not_found / already_exists) — it branches on existence, so neither
// condition is possible by construction.
CliResult doUpsert(TsDevLib& lib, ComponentType type,
                   const std::string& typeStr, const std::string& name,
                   const std::string& source, bool flushPerItem = true) {
    int progId = lib.findExistPos(name, type);
    // §7.1 diag: "findExistPos" batch-phase marker (upsert branch point).
    TSDEV_DIAG_LOG("batch.phase.findExistPos", "item=%s progId=%d",
                   name.c_str(), progId);
    CliResult r;
    if (progId > 0) {
        r = doModify(lib, type, typeStr, name, source);
    } else {
        r = doInsert(lib, type, typeStr, name, source, flushPerItem);
    }
    r.op = "upsert";
    return r;
}

// Parameters passed to the worker thread.
struct InsertParams {
    ComponentType  type;
    std::string    typeStr;
    std::string    name;
    std::string    source;
    const wchar_t* tsPathOverride;  // nullptr = auto-discover
    bool           jsonMode = false;  // cmd* output mode — needed only on
    bool           pbar = false;      // the §5 terminate path (worker-side emit)
    CliResult      result;          // populated by runInsert
};

// The INSERT flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runInsert(LPVOID param) {
    installThreadFatalContainment();  // P4-e: per-thread terminate guard
    InsertParams* p = static_cast<InsertParams*>(param);

    // Construct TsDevLib.
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        CliResult& r = p->result;
        r.op = "insert";
        r.typeStr = p->typeStr;
        r.name = p->name;
        r.exitCode = 4;
        r.errorMessage = std::string("init failed: ") + e.what();
        r.humanStderr = std::string("tsdev_cli: init failed: ") + e.what() + "\n";
        return 0;
    }
    // P4-e: orlib init inside the ctor may have installed its own UEF —
    // re-assert ours outermost before any warehouse call (idempotent).
    installProcessUef();

    p->result = doInsert(*lib, p->type, p->typeStr, p->name, p->source,
                         /*flushPerItem=*/true);

    // §5 teardown: on a flagged possibly-open session this emits the result
    // + TerminateProcess on THIS worker — never reached otherwise.
    singleOpTerminateIfSessionOpen(p->result, p->typeStr, p->name,
                                   p->jsonMode, p->pbar);

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

// --- The INSERT subcommand (parses args, validates, launches worker) ---
int cmdInsert(const std::string& typeStr,
              const std::string& name,
              const std::string& sourceFile,
              const wchar_t* tsPathOverride,
              bool jsonMode, bool pbar) {
    CliResult r;
    r.op = "insert";
    r.typeStr = typeStr;
    r.name = name;

    ComponentType type;
    if (!parseType(typeStr, type)) {
        r.exitCode = 2;
        r.errorMessage = std::string("unknown type \"") + typeStr
                         + "\" (expected strategy/function/indicator/tradingapp/showme)";
        r.humanStderr = std::string("tsdev_cli: unknown type \"") + typeStr
                        + "\" (expected strategy/function/indicator/tradingapp/showme)\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    if (name.empty()) {
        r.exitCode = 2;
        r.errorMessage = "component name must not be empty";
        r.humanStderr = "tsdev_cli: component name must not be empty\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    // 1. Read the source file (on the main thread — no orlib dependency).
    std::string source;
    if (!readFile(sourceFile, source)) {
        r.exitCode = 3;
        r.errorMessage = std::string("failed to read source file \"") + sourceFile + "\"";
        r.humanStderr = std::string("tsdev_cli: failed to read source file \"")
                        + sourceFile + "\"\n";
        emitOutput(r, jsonMode);
        return 3;
    }
    if (source.empty()) {
        r.exitCode = 3;
        r.errorMessage = "source file is empty";
        r.humanStderr = "tsdev_cli: source file is empty\n";
        emitOutput(r, jsonMode);
        return 3;
    }

    // Run the orlib flow on a 64 MB-stack worker thread (the compiler needs
    // a large stack and the EDIT control is thread-affine).
    InsertParams params;
    params.type    = type;
    params.typeStr = typeStr;
    params.name    = name;
    params.source  = source;
    params.tsPathOverride = tsPathOverride;
    params.jsonMode = jsonMode;
    params.pbar     = pbar;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runInsert, &params, 0, nullptr);
    if (!hThread) {
        r.exitCode = 10;
        r.errorMessage = std::string("CreateThread failed: ")
                         + std::to_string(GetLastError());
        r.humanStderr = std::string("tsdev_cli: CreateThread failed: ")
                        + std::to_string(GetLastError()) + "\n";
        emitOutput(r, jsonMode);
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    // --pbar: the op ran to completion — emit the single-item bar in one
    // draw sequence: 0/1 labelled <type> "<name>", then filled to 1/1
    // (with " — 1 failed" on failure), pinned by a newline. stderr only;
    // stdout and the result contract are untouched.
    if (pbar) {
        PbarRenderer bar("insert", 1);
        bar.itemStarted(typeStr, name);
        if (!params.result.success) bar.noteFailure();
        bar.itemCompleted();
        bar.finish();
    }

    // --pbar: a clean success needs no further output — the committed bar
    // line is the whole report. Failures still emit their detail below.
    if (!(pbar && params.result.success)) {
        emitOutput(params.result, jsonMode);
    }
    return params.result.exitCode;
}

// --- The MODIFY subcommand ---

// Parameters passed to the worker thread.
struct ModifyParams {
    ComponentType  type;
    std::string    typeStr;
    std::string    name;
    std::string    source;
    const wchar_t* tsPathOverride;  // nullptr = auto-discover
    CliResult      result;          // populated by runModify
};

// The MODIFY flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runModify(LPVOID param) {
    installThreadFatalContainment();  // P4-e: per-thread terminate guard
    ModifyParams* p = static_cast<ModifyParams*>(param);

    // Construct TsDevLib.
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        CliResult& r = p->result;
        r.op = "modify";
        r.typeStr = p->typeStr;
        r.name = p->name;
        r.exitCode = 4;
        r.errorMessage = std::string("init failed: ") + e.what();
        r.humanStderr = std::string("tsdev_cli: init failed: ") + e.what() + "\n";
        return 0;
    }
    // P4-e: orlib init inside the ctor may have installed its own UEF —
    // re-assert ours outermost before any warehouse call (idempotent).
    installProcessUef();

    p->result = doModify(*lib, p->type, p->typeStr, p->name, p->source);

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

int cmdModify(const std::string& typeStr,
              const std::string& name,
              const std::string& sourceFile,
              const wchar_t* tsPathOverride,
              bool jsonMode, bool pbar) {
    CliResult r;
    r.op = "modify";
    r.typeStr = typeStr;
    r.name = name;

    ComponentType type;
    if (!parseType(typeStr, type)) {
        r.exitCode = 2;
        r.errorMessage = std::string("unknown type \"") + typeStr
                         + "\" (expected strategy/function/indicator/tradingapp/showme)";
        r.humanStderr = std::string("tsdev_cli: unknown type \"") + typeStr
                        + "\" (expected strategy/function/indicator/tradingapp/showme)\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    if (name.empty()) {
        r.exitCode = 2;
        r.errorMessage = "component name must not be empty";
        r.humanStderr = "tsdev_cli: component name must not be empty\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    // 1. Read the source file (on the main thread — no orlib dependency).
    std::string source;
    if (!readFile(sourceFile, source)) {
        r.exitCode = 3;
        r.errorMessage = std::string("failed to read source file \"") + sourceFile + "\"";
        r.humanStderr = std::string("tsdev_cli: failed to read source file \"")
                        + sourceFile + "\"\n";
        emitOutput(r, jsonMode);
        return 3;
    }
    if (source.empty()) {
        r.exitCode = 3;
        r.errorMessage = "source file is empty";
        r.humanStderr = "tsdev_cli: source file is empty\n";
        emitOutput(r, jsonMode);
        return 3;
    }

    // Run the orlib flow on a 64 MB-stack worker thread (the compiler needs
    // a large stack and the EDIT control is thread-affine).
    ModifyParams params;
    params.type     = type;
    params.typeStr  = typeStr;
    params.name     = name;
    params.source   = source;
    params.tsPathOverride = tsPathOverride;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runModify, &params, 0, nullptr);
    if (!hThread) {
        r.exitCode = 10;
        r.errorMessage = std::string("CreateThread failed: ")
                         + std::to_string(GetLastError());
        r.humanStderr = std::string("tsdev_cli: CreateThread failed: ")
                        + std::to_string(GetLastError()) + "\n";
        emitOutput(r, jsonMode);
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    // --pbar: the op ran to completion — emit the single-item bar in one
    // draw sequence: 0/1 labelled <type> "<name>", then filled to 1/1
    // (with " — 1 failed" on failure), pinned by a newline. stderr only;
    // stdout and the result contract are untouched.
    if (pbar) {
        PbarRenderer bar("modify", 1);
        bar.itemStarted(typeStr, name);
        if (!params.result.success) bar.noteFailure();
        bar.itemCompleted();
        bar.finish();
    }

    // --pbar: a clean success needs no further output — the committed bar
    // line is the whole report. Failures still emit their detail below.
    if (!(pbar && params.result.success)) {
        emitOutput(params.result, jsonMode);
    }
    return params.result.exitCode;
}

// --- The DELETE subcommand ---

// Parameters passed to the worker thread.
struct DeleteParams {
    ComponentType  type;
    std::string    typeStr;
    std::string    name;
    const wchar_t* tsPathOverride;  // nullptr = auto-discover
    bool           jsonMode = false;  // cmd* output mode — needed only on
    bool           pbar = false;      // the §5 terminate path (worker-side emit)
    CliResult      result;          // populated by runDelete
};

// The DELETE flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runDelete(LPVOID param) {
    installThreadFatalContainment();  // P4-e: per-thread terminate guard
    DeleteParams* p = static_cast<DeleteParams*>(param);

    // Construct TsDevLib.
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        CliResult& r = p->result;
        r.op = "delete";
        r.typeStr = p->typeStr;
        r.name = p->name;
        r.exitCode = 4;
        r.errorMessage = std::string("init failed: ") + e.what();
        r.humanStderr = std::string("tsdev_cli: init failed: ") + e.what() + "\n";
        return 0;
    }
    // P4-e: orlib init inside the ctor may have installed its own UEF —
    // re-assert ours outermost before any warehouse call (idempotent).
    installProcessUef();

    p->result = doDelete(*lib, p->type, p->typeStr, p->name);

    // §5 teardown: on a flagged possibly-open session this emits the result
    // + TerminateProcess on THIS worker — never reached otherwise.
    singleOpTerminateIfSessionOpen(p->result, p->typeStr, p->name,
                                   p->jsonMode, p->pbar);

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

int cmdDelete(const std::string& typeStr, const std::string& name,
              const wchar_t* tsPathOverride, bool jsonMode, bool pbar) {
    CliResult r;
    r.op = "delete";
    r.typeStr = typeStr;
    r.name = name;

    ComponentType type;
    if (!parseType(typeStr, type)) {
        r.exitCode = 2;
        r.errorMessage = std::string("unknown type \"") + typeStr
                         + "\" (expected strategy/function/indicator/tradingapp/showme)";
        r.humanStderr = std::string("tsdev_cli: unknown type \"") + typeStr
                        + "\" (expected strategy/function/indicator/tradingapp/showme)\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    if (name.empty()) {
        r.exitCode = 2;
        r.errorMessage = "component name must not be empty";
        r.humanStderr = "tsdev_cli: component name must not be empty\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    // Run the orlib flow on a 64 MB-stack worker thread.
    DeleteParams params;
    params.type     = type;
    params.typeStr  = typeStr;
    params.name     = name;
    params.tsPathOverride = tsPathOverride;
    params.jsonMode = jsonMode;
    params.pbar     = pbar;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runDelete, &params, 0, nullptr);
    if (!hThread) {
        r.exitCode = 10;
        r.errorMessage = std::string("CreateThread failed: ")
                         + std::to_string(GetLastError());
        r.humanStderr = std::string("tsdev_cli: CreateThread failed: ")
                        + std::to_string(GetLastError()) + "\n";
        emitOutput(r, jsonMode);
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    // --pbar: the op ran to completion — emit the single-item bar in one
    // draw sequence: 0/1 labelled <type> "<name>", then filled to 1/1
    // (with " — 1 failed" on failure), pinned by a newline. stderr only;
    // stdout and the result contract are untouched.
    if (pbar) {
        PbarRenderer bar("delete", 1);
        bar.itemStarted(typeStr, name);
        if (!params.result.success) bar.noteFailure();
        bar.itemCompleted();
        bar.finish();
    }

    // --pbar: a clean success needs no further output — the committed bar
    // line is the whole report. Failures still emit their detail below.
    if (!(pbar && params.result.success)) {
        emitOutput(params.result, jsonMode);
    }
    return params.result.exitCode;
}

// --- The EXISTS subcommand ---

// Parameters passed to the worker thread.
struct ExistsParams {
    ComponentType  type;
    std::string    typeStr;
    std::string    name;
    bool           caseInsensitive;
    const wchar_t* tsPathOverride;  // nullptr = auto-discover
    CliResult      result;          // populated by runExists
};

// The EXISTS flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runExists(LPVOID param) {
    installThreadFatalContainment();  // P4-e: per-thread terminate guard
    ExistsParams* p = static_cast<ExistsParams*>(param);

    // Construct TsDevLib.
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        CliResult& r = p->result;
        r.op = "exists";
        r.typeStr = p->typeStr;
        r.name = p->name;
        r.exitCode = 4;
        r.errorMessage = std::string("init failed: ") + e.what();
        r.humanStderr = std::string("tsdev_cli: init failed: ") + e.what() + "\n";
        return 0;
    }
    // P4-e: orlib init inside the ctor may have installed its own UEF —
    // re-assert ours outermost before any warehouse call (idempotent).
    installProcessUef();

    p->result = doExists(*lib, p->type, p->typeStr, p->name, p->caseInsensitive);

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

int cmdExists(const std::string& typeStr, const std::string& name,
              bool caseInsensitive,
              const wchar_t* tsPathOverride, bool jsonMode, bool pbar) {
    CliResult r;
    r.op = "exists";
    r.typeStr = typeStr;
    r.name = name;

    ComponentType type;
    if (!parseType(typeStr, type)) {
        r.exitCode = 2;
        r.errorMessage = std::string("unknown type \"") + typeStr
                         + "\" (expected strategy/function/indicator/tradingapp/showme)";
        r.humanStderr = std::string("tsdev_cli: unknown type \"") + typeStr
                        + "\" (expected strategy/function/indicator/tradingapp/showme)\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    if (name.empty()) {
        r.exitCode = 2;
        r.errorMessage = "component name must not be empty";
        r.humanStderr = "tsdev_cli: component name must not be empty\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    // Run the orlib flow on a 64 MB-stack worker thread.
    ExistsParams params;
    params.type     = type;
    params.typeStr  = typeStr;
    params.name     = name;
    params.caseInsensitive = caseInsensitive;
    params.tsPathOverride = tsPathOverride;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runExists, &params, 0, nullptr);
    if (!hThread) {
        r.exitCode = 10;
        r.errorMessage = std::string("CreateThread failed: ")
                         + std::to_string(GetLastError());
        r.humanStderr = std::string("tsdev_cli: CreateThread failed: ")
                        + std::to_string(GetLastError()) + "\n";
        emitOutput(r, jsonMode);
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    // --pbar: the op ran to completion — emit the single-item bar in one
    // draw sequence: 0/1 labelled <type> "<name>", then filled to 1/1
    // (with " — 1 failed" on failure), pinned by a newline. stderr only;
    // stdout and the result contract are untouched.
    if (pbar) {
        PbarRenderer bar("exists", 1);
        bar.itemStarted(typeStr, name);
        if (!params.result.success) bar.noteFailure();
        bar.itemCompleted();
        bar.finish();
    }

    emitOutput(params.result, jsonMode);
    return params.result.exitCode;
}

// --- The DEPENDENTS subcommand (M.4b.1) ---

// doDependents: enumerate reverse dependents (callers) of a library item by
// name + type. This is a READ-ONLY query (no compilation, no modification).
// Returns exit 0 on success (even if zero callers are found — empty is a
// valid result, not an error). Returns exit 5 if the root item is not found.
CliResult doDependents(TsDevLib& lib, ComponentType type,
                       const std::string& typeStr, const std::string& name) {
    CliResult r;
    r.op = "dependents";
    r.typeStr = typeStr;
    r.name = name;

    // First check the root item exists (enumerateRecords is case-insensitive
    // and returns the verbatim stored name + ProgId).
    EnumerateResult er = lib.enumerateRecords(type, name);
    if (!er.exists) {
        r.success = false;
        r.exitCode = 5;
        r.errorMessage = "component not found";
        r.humanStderr = std::string("tsdev_cli: ") + typeStr + " \"" + name
                        + "\" not found\n";
        return r;
    }

    // Enumerate callers via the warehouse-level cmd 0x0F → cmd 0x3B path.
    // Use enumerateCallersByProgId directly with the ProgId from
    // enumerateRecords, bypassing findExistPos (which fails for some
    // existing items due to a BST search issue — see M4B1_HANDOFF §9 gap #2).
    std::vector<TsDevLib::CallerInfo> callers =
        lib.enumerateCallersByProgId(type, er.progId);

    r.success = true;
    r.exitCode = 0;
    r.progId = er.progId;
    r.storedName = er.storedName;

    // Human output: one line per caller: name=... type=... progId=...
    std::string human;
    human += "exists=1\n";
    human += "stored_name=" + er.storedName + "\n";
    human += "progId=" + std::to_string(er.progId) + "\n";
    human += "dependents_count=" + std::to_string(callers.size()) + "\n";
    for (const auto& c : callers) {
        human += "dependent name=" + c.name
               + " type=" + std::to_string(c.category)
               + " progId=" + std::to_string(c.progId) + "\n";
    }
    r.humanStdout = human;

    // JSON output: the callers array is serialized in cliResultToJson via
    // the r.callers field.
    r.callers = callers;

    return r;
}

// Parameters passed to the worker thread.
struct DependentsParams {
    ComponentType  type;
    std::string    typeStr;
    std::string    name;
    const wchar_t* tsPathOverride;  // nullptr = auto-discover
    CliResult      result;          // populated by runDependents
};

// The DEPENDENTS flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runDependents(LPVOID param) {
    installThreadFatalContainment();  // P4-e: per-thread terminate guard
    DependentsParams* p = static_cast<DependentsParams*>(param);

    // Construct TsDevLib.
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        CliResult& r = p->result;
        r.op = "dependents";
        r.typeStr = p->typeStr;
        r.name = p->name;
        r.exitCode = 4;
        r.errorMessage = std::string("init failed: ") + e.what();
        r.humanStderr = std::string("tsdev_cli: init failed: ") + e.what() + "\n";
        return 0;
    }
    // P4-e: orlib init inside the ctor may have installed its own UEF —
    // re-assert ours outermost before any warehouse call (idempotent).
    installProcessUef();

    p->result = doDependents(*lib, p->type, p->typeStr, p->name);

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

int cmdDependents(const std::string& typeStr, const std::string& name,
                  const wchar_t* tsPathOverride, bool jsonMode, bool pbar) {
    CliResult r;
    r.op = "dependents";
    r.typeStr = typeStr;
    r.name = name;

    ComponentType type;
    if (!parseType(typeStr, type)) {
        r.exitCode = 2;
        r.errorMessage = std::string("unknown type \"") + typeStr
                         + "\" (expected strategy/function/indicator/tradingapp/showme)";
        r.humanStderr = std::string("tsdev_cli: unknown type \"") + typeStr
                        + "\" (expected strategy/function/indicator/tradingapp/showme)\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    if (name.empty()) {
        r.exitCode = 2;
        r.errorMessage = "component name must not be empty";
        r.humanStderr = "tsdev_cli: component name must not be empty\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    // Run the orlib flow on a 64 MB-stack worker thread.
    DependentsParams params;
    params.type     = type;
    params.typeStr  = typeStr;
    params.name     = name;
    params.tsPathOverride = tsPathOverride;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runDependents, &params, 0, nullptr);
    if (!hThread) {
        r.exitCode = 10;
        r.errorMessage = std::string("CreateThread failed: ")
                         + std::to_string(GetLastError());
        r.humanStderr = std::string("tsdev_cli: CreateThread failed: ")
                        + std::to_string(GetLastError()) + "\n";
        emitOutput(r, jsonMode);
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    // --pbar: the op ran to completion — emit the single-item bar in one
    // draw sequence: 0/1 labelled <type> "<name>", then filled to 1/1
    // (with " — 1 failed" on failure), pinned by a newline. stderr only;
    // stdout and the result contract are untouched.
    if (pbar) {
        PbarRenderer bar("dependents", 1);
        bar.itemStarted(typeStr, name);
        if (!params.result.success) bar.noteFailure();
        bar.itemCompleted();
        bar.finish();
    }

    emitOutput(params.result, jsonMode);
    return params.result.exitCode;
}

// --- The UPSERT subcommand ---

// Parameters passed to the worker thread.
struct UpsertParams {
    ComponentType  type;
    std::string    typeStr;
    std::string    name;
    std::string    source;
    const wchar_t* tsPathOverride;  // nullptr = auto-discover
    bool           jsonMode = false;  // cmd* output mode — needed only on
    bool           pbar = false;      // the §5 terminate path (worker-side emit)
    CliResult      result;          // populated by runUpsert
};

// The UPSERT flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runUpsert(LPVOID param) {
    installThreadFatalContainment();  // P4-e: per-thread terminate guard
    UpsertParams* p = static_cast<UpsertParams*>(param);

    // Construct TsDevLib.
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        CliResult& r = p->result;
        r.op = "upsert";
        r.typeStr = p->typeStr;
        r.name = p->name;
        r.exitCode = 4;
        r.errorMessage = std::string("init failed: ") + e.what();
        r.humanStderr = std::string("tsdev_cli: init failed: ") + e.what() + "\n";
        return 0;
    }
    // P4-e: orlib init inside the ctor may have installed its own UEF —
    // re-assert ours outermost before any warehouse call (idempotent).
    installProcessUef();

    p->result = doUpsert(*lib, p->type, p->typeStr, p->name, p->source);

    // §5 teardown: on a flagged possibly-open session this emits the result
    // + TerminateProcess on THIS worker — never reached otherwise.
    singleOpTerminateIfSessionOpen(p->result, p->typeStr, p->name,
                                   p->jsonMode, p->pbar);

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

int cmdUpsert(const std::string& typeStr,
              const std::string& name,
              const std::string& sourceFile,
              const wchar_t* tsPathOverride,
              bool jsonMode, bool pbar) {
    CliResult r;
    r.op = "upsert";
    r.typeStr = typeStr;
    r.name = name;

    ComponentType type;
    if (!parseType(typeStr, type)) {
        r.exitCode = 2;
        r.errorMessage = std::string("unknown type \"") + typeStr
                         + "\" (expected strategy/function/indicator/tradingapp/showme)";
        r.humanStderr = std::string("tsdev_cli: unknown type \"") + typeStr
                        + "\" (expected strategy/function/indicator/tradingapp/showme)\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    if (name.empty()) {
        r.exitCode = 2;
        r.errorMessage = "component name must not be empty";
        r.humanStderr = "tsdev_cli: component name must not be empty\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    // 1. Read the source file (on the main thread — no orlib dependency).
    std::string source;
    if (!readFile(sourceFile, source)) {
        r.exitCode = 3;
        r.errorMessage = std::string("failed to read source file \"") + sourceFile + "\"";
        r.humanStderr = std::string("tsdev_cli: failed to read source file \"")
                        + sourceFile + "\"\n";
        emitOutput(r, jsonMode);
        return 3;
    }
    if (source.empty()) {
        r.exitCode = 3;
        r.errorMessage = "source file is empty";
        r.humanStderr = "tsdev_cli: source file is empty\n";
        emitOutput(r, jsonMode);
        return 3;
    }

    // Run the orlib flow on a 64 MB-stack worker thread (the compiler needs
    // a large stack and the EDIT control is thread-affine).
    UpsertParams params;
    params.type    = type;
    params.typeStr = typeStr;
    params.name    = name;
    params.source  = source;
    params.tsPathOverride = tsPathOverride;
    params.jsonMode = jsonMode;
    params.pbar     = pbar;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runUpsert, &params, 0, nullptr);
    if (!hThread) {
        r.exitCode = 10;
        r.errorMessage = std::string("CreateThread failed: ")
                         + std::to_string(GetLastError());
        r.humanStderr = std::string("tsdev_cli: CreateThread failed: ")
                        + std::to_string(GetLastError()) + "\n";
        emitOutput(r, jsonMode);
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    // --pbar: the op ran to completion — emit the single-item bar in one
    // draw sequence: 0/1 labelled <type> "<name>", then filled to 1/1
    // (with " — 1 failed" on failure), pinned by a newline. stderr only;
    // stdout and the result contract are untouched.
    if (pbar) {
        PbarRenderer bar("upsert", 1);
        bar.itemStarted(typeStr, name);
        if (!params.result.success) bar.noteFailure();
        bar.itemCompleted();
        bar.finish();
    }

    // --pbar: a clean success needs no further output — the committed bar
    // line is the whole report. Failures still emit their detail below.
    if (!(pbar && params.result.success)) {
        emitOutput(params.result, jsonMode);
    }
    return params.result.exitCode;
}

// --- The GET-SOURCE subcommand ---

// doGetSource: retrieve the EasyLanguage source text for an existing item by
// name + type. getSource returns the source string; an EMPTY string means the
// item was not found OR the source is unavailable (internal error). Otherwise
// the source is returned. Internally encrypted items are decrypted
// automatically. When an --output file is requested, the source is written to
// that file (on the worker thread); a write failure maps to exit 3 /
// output_write_error.
CliResult doGetSource(TsDevLib& lib, ComponentType type,
                      const std::string& typeStr, const std::string& name,
                      const std::string& outputPath) {
    CliResult r;
    r.op = "get-source";
    r.typeStr = typeStr;
    r.name = name;

    std::string source = lib.getSource(type, name);
    if (source.empty()) {
        r.exitCode = 5;
        r.errorMessage = typeStr + " \"" + name
                         + "\" not found or source unavailable.";
        r.humanStderr = std::string("tsdev_cli: ERROR — ") + typeStr + " \""
                        + name + "\" not found or source unavailable.\n";
        return r;
    }

    // Source retrieved successfully.
    r.success = true;
    r.exitCode = 0;
    r.sourceText = source;

    if (!outputPath.empty()) {
        // Write the source to the requested file.
        if (!writeFile(outputPath, source)) {
            r.success = false;
            r.exitCode = 3;
            r.sourceText.clear();
            r.errorMessage = std::string("failed to write source to output file \"")
                             + outputPath + "\"";
            r.humanStderr = std::string("tsdev_cli: failed to write source to "
                            "output file \"") + outputPath + "\"\n";
            return r;
        }
        // File written — human-readable stdout reports the destination, NOT
        // the source text. JSON mode still carries the source in `source`.
        r.humanStdout = std::string("Source written to ") + outputPath + "\n";
    } else {
        // stdout mode: emit the raw source text directly (no wrapper).
        r.humanStdout = source;
    }
    return r;
}

// Parameters passed to the worker thread.
struct GetSourceParams {
    ComponentType  type;
    std::string    typeStr;
    std::string    name;
    std::string    outputPath;  // empty = stdout mode
    const wchar_t* tsPathOverride;  // nullptr = auto-discover
    CliResult      result;          // populated by runGetSource
};

// The GET-SOURCE flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runGetSource(LPVOID param) {
    installThreadFatalContainment();  // P4-e: per-thread terminate guard
    GetSourceParams* p = static_cast<GetSourceParams*>(param);

    // Construct TsDevLib.
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        CliResult& r = p->result;
        r.op = "get-source";
        r.typeStr = p->typeStr;
        r.name = p->name;
        r.exitCode = 4;
        r.errorMessage = std::string("init failed: ") + e.what();
        r.humanStderr = std::string("tsdev_cli: init failed: ") + e.what() + "\n";
        return 0;
    }
    // P4-e: orlib init inside the ctor may have installed its own UEF —
    // re-assert ours outermost before any warehouse call (idempotent).
    installProcessUef();

    p->result = doGetSource(*lib, p->type, p->typeStr, p->name, p->outputPath);

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

int cmdGetSource(const std::string& typeStr, const std::string& name,
                 const std::string& outputPath,
                 const wchar_t* tsPathOverride, bool jsonMode, bool pbar) {
    CliResult r;
    r.op = "get-source";
    r.typeStr = typeStr;
    r.name = name;

    ComponentType type;
    if (!parseType(typeStr, type)) {
        r.exitCode = 2;
        r.errorMessage = std::string("unknown type \"") + typeStr
                         + "\" (expected strategy/function/indicator/tradingapp/showme)";
        r.humanStderr = std::string("tsdev_cli: unknown type \"") + typeStr
                        + "\" (expected strategy/function/indicator/tradingapp/showme)\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    if (name.empty()) {
        r.exitCode = 2;
        r.errorMessage = "component name must not be empty";
        r.humanStderr = "tsdev_cli: component name must not be empty\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    // Run the orlib flow on a 64 MB-stack worker thread.
    GetSourceParams params;
    params.type     = type;
    params.typeStr  = typeStr;
    params.name     = name;
    params.outputPath = outputPath;
    params.tsPathOverride = tsPathOverride;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runGetSource, &params, 0, nullptr);
    if (!hThread) {
        r.exitCode = 10;
        r.errorMessage = std::string("CreateThread failed: ")
                         + std::to_string(GetLastError());
        r.humanStderr = std::string("tsdev_cli: CreateThread failed: ")
                        + std::to_string(GetLastError()) + "\n";
        emitOutput(r, jsonMode);
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    // --pbar: the op ran to completion — emit the single-item bar in one
    // draw sequence: 0/1 labelled <type> "<name>", then filled to 1/1
    // (with " — 1 failed" on failure), pinned by a newline. stderr only;
    // stdout and the result contract are untouched.
    if (pbar) {
        PbarRenderer bar("get-source", 1);
        bar.itemStarted(typeStr, name);
        if (!params.result.success) bar.noteFailure();
        bar.itemCompleted();
        bar.finish();
    }

    emitOutput(params.result, jsonMode);
    return params.result.exitCode;
}

// --- TVS-D.3: batch mode ----------------------------------------------------
//
// Three batch subcommands mirror the single-item subcommands but process a
// manifest file containing one item per line:
//   batch-insert <manifest_file>   lines: <type> <name> <source_file>
//   batch-modify <manifest_file>   lines: <type> <name> <source_file>
//   batch-delete <manifest_file>   lines: <type> <name>
//
// ONE TsDevLib instance is constructed on a single 64 MB-stack worker thread
// and reused for ALL items (amortizing the ~411ms init). Output is ALWAYS a
// JSON array on stdout (one element per real manifest item, in order). Exit
// codes: 0 = all succeeded, 1 = usage/manifest error (batch did not run),
// 2 = batch ran but one or more items failed.

// A single parsed manifest item. The op (insert/modify/delete) is fixed by
// the subcommand; each item carries only the per-item args.
struct BatchItem {
    ComponentType type;
    std::string   typeStr;     // verbatim <type> token (for JSON output)
    std::string   name;
    std::string   sourceFile;  // insert/modify only (empty for delete)
    std::string   source;      // pre-read source text (insert/modify only)
    bool          preValid = true;   // false = malformed line / unreadable file
    CliResult     result;      // pre-populated for preValid=false items
};

// The ONE whitespace class of the manifest line schema: exactly the
// C-locale std::isspace set (" \t\n\v\f\r"). trim(), token splitting, and
// the resume-manifest writer's quoting/refusal predicates ALL use this
// same set, so a name the writer emits can never re-parse to a different
// component — a '\v'/'\f' adjacent to a token boundary must quote or
// refuse, never silently retarget (a resumed delete would hit the wrong
// item).
const char* const kManifestWs = " \t\n\v\f\r";
// The kManifestWs subset that can never live on one manifest line — a
// name containing '\r' or '\n' can never round-trip (a line break ends
// the record), so the resume-manifest writer refuses it outright.
const char* const kManifestLineBreaks = "\r\n";
bool isManifestWs(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

// trim from both ends (manifest whitespace only — the kManifestWs class
// above; std::isspace in the C locale is that same 6-char set).
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isManifestWs(s[b])) ++b;
    while (e > b && isManifestWs(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// Parse manifest content (a single string with newline-separated lines) into
// BatchItems. Used by both the file-path mode (parseManifest) and the
// --batch-stdin mode. Always succeeds (parsing a string cannot fail to open).
void parseManifestLines(const std::string& content, bool needsSource,
                        std::vector<BatchItem>& outItems) {
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        // Strip a trailing '\r' (manifest may use CRLF).
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = trim(line);
        // Skip blank lines and comments.
        if (trimmed.empty()) continue;
        if (trimmed[0] == '#') continue;

        BatchItem item;

        // Token 1: <type> (first whitespace-delimited token).
        size_t sp1 = trimmed.find_first_of(kManifestWs);
        std::string typeTok;
        std::string rest1;
        if (sp1 == std::string::npos) {
            typeTok = trimmed;
            rest1 = "";
        } else {
            typeTok = trimmed.substr(0, sp1);
            rest1 = trim(trimmed.substr(sp1 + 1));
        }

        // Token 2: <name> — first whitespace-delimited token of the
        // remainder, OR a double-quoted string ("my name") so a resume
        // manifest can carry names containing whitespace (directory
        // batches derive the name verbatim from the file stem, which may
        // contain spaces). An unterminated quote, or a closing quote not
        // followed by whitespace/end-of-line (junk fused to the quoted
        // token, e.g. "a"b), is a pre-validation failure — fail-closed:
        // silently re-tokenizing would address a DIFFERENT component (a
        // delete would hit the wrong item).
        std::string nameTok;
        std::string rest2;
        if (!rest1.empty() && rest1[0] == '"') {
            const size_t close = rest1.find('"', 1);
            if (close == std::string::npos) {
                item.typeStr = typeTok;
                item.preValid = false;
                item.result.typeStr = typeTok;
                item.result.exitCode = 2;
                item.result.errorMessage =
                    "unterminated quoted component name in manifest line: \"" +
                    trimmed + "\"";
                outItems.push_back(std::move(item));
                continue;
            }
            if (close + 1 < rest1.size() && !isManifestWs(rest1[close + 1])) {
                item.typeStr = typeTok;
                item.preValid = false;
                item.result.typeStr = typeTok;
                item.result.exitCode = 2;
                item.result.errorMessage =
                    "malformed quoted component name in manifest line "
                    "(characters after the closing quote): \"" + trimmed +
                    "\"";
                outItems.push_back(std::move(item));
                continue;
            }
            nameTok = rest1.substr(1, close - 1);
            rest2 = trim(rest1.substr(close + 1));
        } else {
            size_t sp2 = rest1.find_first_of(kManifestWs);
            if (sp2 == std::string::npos) {
                nameTok = rest1;
                rest2 = "";
            } else {
                nameTok = rest1.substr(0, sp2);
                rest2 = trim(rest1.substr(sp2 + 1));
            }
        }

        item.typeStr = typeTok;
        item.name = nameTok;

        // Validate <type>.
        ComponentType ctype;
        if (!parseType(typeTok, ctype)) {
            item.preValid = false;
            item.result.op = "";  // op set by caller (cmdBatch*) after parse
            item.result.typeStr = typeTok;
            item.result.name = nameTok;
            item.result.exitCode = 2;
            item.result.errorMessage = std::string("unknown type \"") + typeTok
                + "\" (expected strategy/function/indicator/tradingapp/showme)";
            outItems.push_back(std::move(item));
            continue;
        }
        item.type = ctype;

        // Validate <name>.
        if (nameTok.empty()) {
            item.preValid = false;
            item.result.typeStr = typeTok;
            item.result.exitCode = 2;
            item.result.errorMessage = "component name must not be empty";
            outItems.push_back(std::move(item));
            continue;
        }

        if (needsSource) {
            // <source_file> = the REST of the line after the second token.
            if (rest2.empty()) {
                item.preValid = false;
                item.result.typeStr = typeTok;
                item.result.name = nameTok;
                item.result.exitCode = 1;
                item.result.errorMessage =
                    "manifest line missing <source_file>: \"" + trimmed + "\"";
                outItems.push_back(std::move(item));
                continue;
            }
            item.sourceFile = rest2;
            // Read the source file now (binary mode, matching single-item).
            if (!readFile(rest2, item.source)) {
                item.preValid = false;
                item.result.typeStr = typeTok;
                item.result.name = nameTok;
                item.result.exitCode = 3;
                item.result.errorMessage =
                    std::string("failed to read source file \"") + rest2 + "\"";
                outItems.push_back(std::move(item));
                continue;
            }
            if (item.source.empty()) {
                item.preValid = false;
                item.result.typeStr = typeTok;
                item.result.name = nameTok;
                item.result.exitCode = 3;
                item.result.errorMessage = "source file is empty";
                outItems.push_back(std::move(item));
                continue;
            }
        }

        item.preValid = true;
        outItems.push_back(std::move(item));
    }
}

// Parse a manifest file into a list of BatchItems. Lines starting with '#'
// (comments) and blank/whitespace-only lines are skipped. Malformed lines
// (wrong token count, unknown type, empty name) and unreadable source files
// (insert/modify) are recorded as pre-validated FAILURE items so the batch
// continues and reports them. Returns false only if the manifest file itself
// cannot be opened (caller reports exit 1).
//
// needsSource: true for batch-insert/batch-modify (3 tokens: type name file),
//              false for batch-delete (2 tokens: type name).
bool parseManifest(const std::string& path, bool needsSource,
                   std::vector<BatchItem>& outItems) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    parseManifestLines(content, needsSource, outItems);
    return true;
}

// --- TVS C: cooperative stdin batch control ---------------------------------
//
// --batch-progress-control turns stdin into a control channel while the
// batch runs. With --batch-stdin the channel opens with a framed manifest:
//   TSDEV_BATCH_CONTROL/1 <decimal byte length>\n<exactly length bytes>
// after which the same stream carries one LF-terminated decision line per
// non-final item ("continue\n" or "cancel\n"). With a manifest path or a
// batch-*-dir subcommand the manifest comes from its normal source and
// stdin carries decision lines only. The worker reads exactly one decision
// synchronously after each completed non-final item; "cancel", EOF, or any
// malformed token stops the batch cooperatively before the next item and
// fills the rest of the results array with exit-12 not-run markers.
//
// R6 (design §5): the decision read is BOUNDED — it runs on a helper
// thread under a WaitForSingleObject bound (default 900 s,
// --batch-control-timeout-secs; 0 disables). A dead parent that left the
// channel open can no longer park the batch in an unbounded orphan wait
// (the ~40 min orphan incident). A bound expiry yields Error = cancel.

// One between-items decision read from the control channel.
enum class BatchControlDecision { Continue, Cancel, Error };

// 64 MiB inclusive cap for the framed manifest payload (the same cap the
// serve framing layer applies to one frame).
static constexpr size_t kBatchControlManifestCap = 67108864;

// readBatchControlManifest reads the framed manifest form used when
// --batch-stdin is combined with --batch-progress-control: the exact ASCII
// header "TSDEV_BATCH_CONTROL/1 <decimal byte length>\n" followed by
// exactly <length> manifest bytes. The length token is decimal digits only
// (no sign, spaces, or trailing junk), parsed overflow-safe, and capped at
// 64 MiB inclusive. Bytes after the payload (decision lines) are left
// unread. Returns false with a nonempty diagnostic on EOF, a malformed
// header, a read failure, or a short payload.
bool readBatchControlManifest(FILE* input, std::string& manifest,
                              std::string& error) {
    static const char kHeaderMagic[] = "TSDEV_BATCH_CONTROL/1 ";
    const size_t kMagicLen = sizeof(kHeaderMagic) - 1;

    // Read the header one byte at a time up to and including the LF. The
    // scan is bounded (magic + at most 8 digits for the cap, plus slack) so
    // a malformed stream cannot grow it unboundedly.
    std::string header;
    header.reserve(64);
    bool gotLf = false;
    for (size_t i = 0; i < 128; ++i) {
        int c = std::fgetc(input);
        if (c == EOF) {
            error = header.empty()
                ? "control channel closed before the batch manifest header"
                : "control channel closed inside the batch manifest header";
            return false;
        }
        if (c == '\n') { gotLf = true; break; }
        header.push_back(static_cast<char>(c));
    }
    if (!gotLf) {
        error = "batch control manifest header exceeds the maximum length";
        return false;
    }
    if (header.compare(0, kMagicLen, kHeaderMagic) != 0) {
        error = "malformed batch control manifest header "
                "(expected \"TSDEV_BATCH_CONTROL/1 <byte length>\")";
        return false;
    }
    const std::string lenTok = header.substr(kMagicLen);
    if (lenTok.empty() ||
        lenTok.find_first_not_of("0123456789") != std::string::npos) {
        error = "batch control manifest length is not a decimal byte count";
        return false;
    }
    // Decimal digits only; bail out above the cap before the value can
    // overflow (the cap is far below size_t range).
    size_t declared = 0;
    for (char ch : lenTok) {
        declared = declared * 10 + static_cast<size_t>(ch - '0');
        if (declared > kBatchControlManifestCap) {
            error = "batch control manifest length exceeds the 64 MiB cap";
            return false;
        }
    }
    manifest.assign(declared, '\0');
    if (declared > 0 &&
        std::fread(&manifest[0], 1, declared, input) != declared) {
        error = "control channel closed before the declared manifest "
                "payload was read";
        return false;
    }
    return true;
}

// readBatchControlDecision reads exactly one decision line from the control
// channel. Only "continue\n" and "cancel\n" are valid. EOF before the line
// terminator, a read error, an oversized token, or any other token yields
// Error with a nonempty diagnostic (the caller treats Error as cancel).
BatchControlDecision readBatchControlDecision(FILE* input,
                                              std::string& error) {
    std::string line;
    line.reserve(16);
    // Bound the token: the only legal lines are "continue\n" and
    // "cancel\n", so anything beyond a small limit is malformed by
    // definition.
    for (size_t i = 0; i < 64; ++i) {
        int c = std::fgetc(input);
        if (c == EOF) {
            error = std::ferror(input)
                ? "error reading the batch control channel"
                : (line.empty()
                    ? "control channel closed before a decision was read"
                    : "control channel closed before the decision line "
                      "terminator");
            return BatchControlDecision::Error;
        }
        if (c == '\n') {
            if (line == "continue") return BatchControlDecision::Continue;
            if (line == "cancel")   return BatchControlDecision::Cancel;
            error = "invalid batch control decision token \"" + line + "\"";
            return BatchControlDecision::Error;
        }
        line.push_back(static_cast<char>(c));
    }
    error = "batch control decision token exceeds the maximum length";
    return BatchControlDecision::Error;
}

// --- Bounded control wait (R6, design §5 row 3) ------------------------------
//
// readBatchControlDecision blocks in fgetc() with no bound. A dead parent
// that left the control channel OPEN (the pipe writer handle never
// closed — e.g. inherited by a still-alive grandchild) keeps EOF from
// ever arriving: the batch waits forever as an orphan (field incident:
// a ~40 min orphan control-wait that then crashed on exit). The read is
// therefore run on a helper thread and the caller waits on the thread
// handle with a bounded WaitForSingleObject (default 900 s;
// --batch-control-timeout-secs; 0 disables the bound and restores the
// plain blocking read — documented escape hatch, same convention as
// --dispatch-timeout-secs). A timeout yields Error, which the existing
// flow treats as a cooperative cancel — fail-closed, the batch ends.
//
// The wait holds no in-flight dispatch marker, so the dispatch watchdog
// is already exempt during it (dispatch_watchdog.cpp: "idle waits ...
// carry no in-flight marker and are exempt by construction").

// Heap-allocated job shared with the helper thread: a timing-out wait
// ABANDONS the blocked reader (fgetc on an open channel cannot be
// cancelled without closing stdin), so the job must outlive the caller —
// the abandoned thread still writes decision/error into it. At most one
// abandoned job per run exists because a timeout cancels the batch.
struct BatchControlReadJob {
    BatchControlDecision (*reader)(void* ctx, std::string& error);
    void*                ctx;
    BatchControlDecision decision;
    std::string          error;
};

unsigned __stdcall batchControlReadProc(void* arg) {
    installThreadFatalContainment();  // P4-e: per-thread terminate guard
    BatchControlReadJob* job = static_cast<BatchControlReadJob*>(arg);
    std::string err;
    job->decision = job->reader(job->ctx, err);
    job->error = std::move(err);
    return 0;
}

// runBoundedControlRead runs `reader(ctx)` on a helper thread and waits up
// to `timeoutMs` for it. On completion the reader's decision and error
// pass through unchanged. On timeout or wait failure the result is Error
// with a nonempty diagnostic (the caller treats Error as cancel); the
// still-blocked helper is deliberately abandoned — its handle is closed
// (closing a thread handle does not kill the thread) and its heap job is
// leaked, bounded at one per run since a timeout cancels the batch; the
// thread dies with the process. The reader is injected so unit tests can
// exercise the real thread+wait machinery without stdin.
BatchControlDecision runBoundedControlRead(
    BatchControlDecision (*reader)(void* ctx, std::string& error),
    void* ctx, DWORD timeoutMs, std::string& error) {
    BatchControlReadJob* job = new (std::nothrow) BatchControlReadJob();
    if (!job) {
        error = "out of memory starting the bounded control reader";
        return BatchControlDecision::Error;
    }
    job->reader   = reader;
    job->ctx      = ctx;
    job->decision = BatchControlDecision::Error;
    // _beginthreadex (not CreateThread): the reader runs CRT calls (fgetc),
    // so the thread needs the CRT per-thread state that _beginthreadex
    // sets up and _endthreadex tears down.
    const uintptr_t th = _beginthreadex(nullptr, 0, batchControlReadProc,
                                        job, 0, nullptr);
    if (th == 0) {
        delete job;
        error = "could not start the bounded control reader thread";
        return BatchControlDecision::Error;
    }
    const HANDLE hThread = reinterpret_cast<HANDLE>(th);
    const DWORD w = WaitForSingleObject(hThread, timeoutMs);
    if (w == WAIT_OBJECT_0) {
        // The reader exited — the handle wait synchronizes-with its exit,
        // so decision/error are fully written. Join-complete: safe to
        // free the job.
        error = std::move(job->error);
        const BatchControlDecision d = job->decision;
        delete job;
        CloseHandle(hThread);
        return d;
    }
    // WAIT_TIMEOUT / WAIT_FAILED: do NOT join (the reader is still blocked
    // on the control channel) and do NOT free the job — the abandoned
    // thread still writes to it.
    if (w == WAIT_TIMEOUT) {
        // Report the bound in its true unit (whole seconds when exact —
        // production bounds are second-valued — else milliseconds so a
        // sub-second bound never renders as "0 s").
        const std::string bound = (timeoutMs % 1000 == 0)
            ? std::to_string(static_cast<unsigned long>(timeoutMs / 1000)) +
              " s"
            : std::to_string(static_cast<unsigned long>(timeoutMs)) + " ms";
        error = "batch control decision wait exceeded the " + bound +
                " bound (control channel stalled or parent gone)";
    } else {
        error = "the bounded batch control wait failed (GetLastError=" +
                std::to_string(static_cast<unsigned long>(GetLastError())) +
                ")";
    }
    CloseHandle(hThread);
    return BatchControlDecision::Error;
}

// FILE* thunk adapting readBatchControlDecision to the injected-reader
// signature runBoundedControlRead takes.
BatchControlDecision batchControlFileReader(void* ctx, std::string& error) {
    return readBatchControlDecision(static_cast<FILE*>(ctx), error);
}

// readBatchControlDecisionBounded: the production control-read entry —
// readBatchControlDecision on a helper thread under a bounded wait.
// timeoutSecs==0 disables the bound (plain blocking read; documented
// escape hatch). Callers cap the value at 86400, so *1000 cannot
// overflow DWORD (~4.29e6 s max representable).
BatchControlDecision readBatchControlDecisionBounded(
    FILE* input, unsigned timeoutSecs, std::string& error) {
    if (timeoutSecs == 0) {
        return readBatchControlDecision(input, error);
    }
    return runBoundedControlRead(batchControlFileReader, input,
                                 static_cast<DWORD>(timeoutSecs) * 1000,
                                 error);
}

// Render the batch_control_error diagnostic emitted on stderr before an
// anomaly-driven cancellation. JSON: exactly {event:"batch_control_error",
// message, action:"cancel"}. Human:
//   BATCH CONTROL ERROR: <message>; cancelling before next item
// where <message> is the JSON one-line escaped form without the
// surrounding quotes (the same escaping jsonQuote applies).
std::string batchControlErrorLine(const std::string& message,
                                  bool jsonOutput) {
    if (jsonOutput) {
        ordered_json j;
        j["event"]   = "batch_control_error";
        j["message"] = message;
        j["action"]  = "cancel";
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    const std::string quoted = jsonQuote(message);
    return "BATCH CONTROL ERROR: " + quoted.substr(1, quoted.size() - 2) +
           "; cancelling before next item";
}

// Synthesize the not-run marker for an item the batch never attempted after
// a cancellation (default message) or a guard-driven abort (override
// message). Preserves the parsed op/type/name verbatim; exit 12
// serializes as error.code "cancelled" via errorCodeString.
CliResult cancelledBatchItemResult(
    const std::string& op, const BatchItem& item,
    const std::string& message =
        "Batch cancelled before this item was attempted") {
    CliResult r;
    r.op = op;
    r.typeStr = item.typeStr;
    r.name = item.name;
    r.success = false;
    r.exitCode = 12;
    r.errorMessage = message;
    return r;
}

// Message used for not-run markers when the WHServer VAS-headroom guard
// aborted the batch (distinct from the cooperative-cancel message so the
// array says WHY the item never ran).
static const char* const kVasAbortMarkerMessage =
    "Batch aborted (WHServer VAS headroom) before this item was attempted";

// Same role for the corpse-abort arm of the guard (P4 gap-a): the batch
// was aborted because a WHServer pid is a crash-frozen zombie — distinct
// text so the array does not misreport a zombie as a VAS breach.
static const char* const kZombieAbortMarkerMessage =
    "Batch aborted (WHServer crash-frozen zombie) before this item was "
    "attempted";

// Render the one-shot warning for a transient measureVasFree() failure
// (emitted once per run — a handle/OpenProcess race must not kill a batch).
// JSON: {event:"whserver_vas_measure_failed", pid}. Human:
//   tsdev_cli: whserver.vas.measure_failed pid=N (continuing)
std::string whserverVasMeasureFailedLine(DWORD pid, bool jsonOutput) {
    if (jsonOutput) {
        ordered_json j;
        j["event"] = "whserver_vas_measure_failed";
        j["pid"]   = static_cast<unsigned long>(pid);
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    return "tsdev_cli: whserver.vas.measure_failed pid=" +
           std::to_string(static_cast<unsigned long>(pid)) + " (continuing)";
}

// Render the loud abort event emitted when the WHServer VAS-headroom guard
// fires (breach semantics: measured value STRICTLY below its threshold).
// JSON: {event:"batch_aborted", reason:"whserver_vas_headroom", pid,
//        free_mb, largest_free_mb, threshold_free_mb,
//        threshold_largest_mb, action:"abort"}. Human:
//   BATCH ABORTED: WHServer VAS headroom below threshold
//   (pid=N free=XMB largest=YMB thresholds free=TMB largest=CMB);
//   aborting before next item
std::string whserverVasAbortLine(DWORD pid, const tsdev::WhVasStats& st,
                                 unsigned thresholdFreeMB,
                                 unsigned thresholdLargestMB,
                                 bool jsonOutput) {
    const uint64_t freeMB    = st.totalFreeBytes        / (1024 * 1024);
    const uint64_t largestMB = st.largestFreeBlockBytes / (1024 * 1024);
    if (jsonOutput) {
        ordered_json j;
        j["event"]                  = "batch_aborted";
        j["reason"]                 = "whserver_vas_headroom";
        j["pid"]                    = static_cast<unsigned long>(pid);
        j["free_mb"]                = freeMB;
        j["largest_free_mb"]        = largestMB;
        j["threshold_free_mb"]      = thresholdFreeMB;
        j["threshold_largest_mb"]   = thresholdLargestMB;
        j["action"]                 = "abort";
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    return "BATCH ABORTED: WHServer VAS headroom below threshold (pid=" +
           std::to_string(static_cast<unsigned long>(pid)) +
           " free=" + std::to_string(freeMB) + "MB" +
           " largest=" + std::to_string(largestMB) + "MB" +
           " thresholds free=" + std::to_string(thresholdFreeMB) + "MB" +
           " largest=" + std::to_string(thresholdLargestMB) + "MB" +
           "); aborting before next item";
}

// Render the loud abort event emitted when the guard's corpse pre-check
// fires (P4 gap-a): a WHServer pid that parents a TSCrashReport.exe child
// is a crash-frozen zombie — it still enumerates and its frozen VAS reads
// healthy under VirtualQueryEx, but every warehouse dispatch into it
// wedges (the dispatch_watchdog whserver_crashed convention). Same
// "batch_aborted" event family as whserverVasAbortLine, with the honest
// "whserver_zombie" reason instead of a VAS measurement.
// JSON: {event:"batch_aborted", reason:"whserver_zombie", pid,
//        action:"abort"}. Human:
//   BATCH ABORTED: WHServer crash-frozen zombie (pid=N parents a
//   TSCrashReport.exe child); aborting before next item
std::string whserverZombieAbortLine(DWORD pid, bool jsonOutput) {
    if (jsonOutput) {
        ordered_json j;
        j["event"]  = "batch_aborted";
        j["reason"] = "whserver_zombie";
        j["pid"]    = static_cast<unsigned long>(pid);
        j["action"] = "abort";
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    return "BATCH ABORTED: WHServer crash-frozen zombie (pid=" +
           std::to_string(static_cast<unsigned long>(pid)) +
           " parents a TSCrashReport.exe child); aborting before next item";
}

// Render the one-shot warning for an inconclusive process enumeration at
// the guard (P4 g-1 — a failed Toolhelp32 snapshot is INCONCLUSIVE, never
// "no server"/"no reporters"; see the snapshotOk contracts in
// whserver_health.h and dispatch_watchdog.h). Emitted once per run per
// enumeration arm — the same fail-open-with-warning convention as
// whserverVasMeasureFailedLine: the guard proceeds with the same
// protection the collapsing overloads gave silently, now honestly
// reported. `which` selects the enumeration:
//   "whserver"    the WHServer pid set itself — the whole guard is blind
//                 this check;
//   "crashreport" the TSCrashReport parent set behind the zombie
//                 pre-check — the corpse check is blind this check; the
//                 VAS measurement still runs.
// JSON: {event:"whserver_enum_failed"} / {event:"crashreport_enum_failed"}.
// Human:
//   tsdev_cli: whserver.enum_failed (guard inconclusive; continuing)
//   tsdev_cli: crashreport.enum_failed (zombie check inconclusive;
//              continuing)
std::string whserverEnumFailedLine(const char* which, bool jsonOutput) {
    const bool wh = (std::strcmp(which, "crashreport") != 0);
    if (jsonOutput) {
        ordered_json j;
        j["event"] =
            wh ? "whserver_enum_failed" : "crashreport_enum_failed";
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    return std::string("tsdev_cli: ") +
           (wh ? "whserver.enum_failed (guard inconclusive; continuing)"
               : "crashreport.enum_failed (zombie check inconclusive; "
                  "continuing)");
}

// The corpse pre-check's pure decision (P4 gap-a): the first WHServer pid
// that also appears as a TSCrashReport.exe PARENT pid — a crash-frozen
// zombie — or 0 when the intersection is empty. TSCrash.dll launches the
// reporter as a child of the crashing process, so parent-in-WHServer-set
// is exactly the dispatch_watchdog whserver_crashed test. Both sets come
// from the caller's snapshotOk enumerations; inconclusive enumerations
// never reach here (they warn and fail open upstream).
DWORD whserverZombiePid(const std::vector<DWORD>& whPids,
                        const std::vector<DWORD>& crashParentPids) {
    for (DWORD parent : crashParentPids) {
        if (std::find(whPids.begin(), whPids.end(), parent) != whPids.end())
            return parent;
    }
    return 0;
}

// The honest refusal message for a detected corpse — mirrors the breach
// message's reason-in-parens convention ("... (whserver_vas_free=NMB);
// refusing <op> before dispatch"): names the frozen pid + its crash
// reporter and carries the machine-readable "whserver_zombie" token the
// batch path reports as abort_reason.
std::string whserverZombieRefusalMessage(const std::string& op,
                                         DWORD zombiePid) {
    return "WHServer crash-frozen zombie: pid=" +
           std::to_string(static_cast<unsigned long>(zombiePid)) +
           " parents a TSCrashReport.exe child (whserver_zombie); "
           "refusing " + op + " before dispatch";
}

// Not-run marker messages for the recycle paths (design §4.1 — the markers
// are distinct from kVasAbortMarkerMessage so the array says WHY the item
// never ran). These are the shipped marker texts — pinned verbatim by the
// Python client (_RECYCLE_REFUSED_MARKER_MESSAGE mirrors the refusal text;
// the "resubmit remaining items" text is the resumable-tail marker the
// client replaces when no resubmission can happen).
static const char* const kRecycledMarkerMessage =
    "WHServer recycled; resubmit remaining items";
static const char* const kRecycleRefusedMarkerMessage =
    "WHServer recycle refused before this item was attempted";

// The recycle event line emitted on the progress channel when the engine
// runs (design §4.1 step 3 — full field set: reason/detail/exit_code plus
// the breaching WHServer pid, measured free_mb, the 1-based resume
// boundary index/total, and the resume-manifest path; each optional
// member is emitted only when its caller supplies it — the serve path and
// the pre-flight continue-in-process event have no boundary/manifest).
// JSON:
//   {event:"whserver_recycled"|"whserver_recycle_refused", reason, detail,
//    exit_code[, pid, free_mb, index, total, manifest]}. Human:
//   tsdev_cli: whserver.recycled / whserver.recycle_refused reason=... ...
std::string whserverRecycleEventLine(const tsdev::RecycleResult& rec,
                                     bool jsonOutput,
                                     DWORD whPid = 0,
                                     uint64_t freeMB = 0,
                                     size_t boundaryIndex = 0,
                                     size_t total = 0,
                                     const std::string& manifestPath = "") {
    const bool recycled =
        (rec.outcome == tsdev::RecycleOutcome::Recycled);
    const char* event = recycled ? "whserver_recycled"
                                 : "whserver_recycle_refused";
    if (jsonOutput) {
        ordered_json j;
        j["event"]     = event;
        j["reason"]    = rec.reason;
        j["detail"]    = rec.detail;
        j["exit_code"] = rec.exitCode;
        if (whPid != 0)   j["pid"]     = static_cast<unsigned long>(whPid);
        if (freeMB != 0)  j["free_mb"] = freeMB;
        if (boundaryIndex != 0) {
            j["index"] = boundaryIndex;
            j["total"] = total;
        }
        if (!manifestPath.empty()) j["manifest"] = manifestPath;
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    std::string s = std::string("tsdev_cli: whserver.") +
        (recycled ? "recycled" : "recycle_refused") +
        " reason=" + rec.reason;
    if (!rec.detail.empty()) s += " " + rec.detail;
    s += " exit=" + std::to_string(rec.exitCode);
    if (whPid != 0) {
        s += " pid=" + std::to_string(static_cast<unsigned long>(whPid));
    }
    if (freeMB != 0) s += " free_mb=" + std::to_string(freeMB);
    if (boundaryIndex != 0) {
        s += " index=" + std::to_string(boundaryIndex) +
             " total=" + std::to_string(total);
    }
    if (!manifestPath.empty()) s += " manifest=" + manifestPath;
    return s;
}

// Resume-manifest writer (design §4.1 step 2): write items[firstIndex, ...)
// to `tsdev_resume_<pid>_<ticks>.manifest` inside `dir` (empty dir falls
// back to the cwd) in the exact schema parseManifestLines re-reads —
// "<type> <name> [<source_file>]" per line, with <name> double-quoted when
// it contains whitespace (directory batches derive names verbatim from
// file stems, which may contain spaces — an unquoted spaced name would
// re-parse to a DIFFERENT component: dropped items, or a delete hitting
// the wrong target). Only truly unrepresentable names make the writer
// REFUSE (fail-closed) before creating any file: a name containing a line
// break can never live on one line, a name STARTING with '"' would
// re-parse as a quoted token, and a name containing BOTH '"' and
// whitespace cannot be expressed (unquoted it splits; quoted it
// terminates early). A bare '"' inside a whitespace-free name IS
// representable unquoted — it re-parses as a plain token — so it is
// written, not refused. On refusal no partial manifest may be
// resubmitted, and the caller's diagnostics state that no manifest
// exists. The rule in one line: a stored preValid=FALSE item refuses
// the whole write BEFORE any file exists (fail-closed — no manifest is
// emitted), while a stored preValid=TRUE item is always written and
// round-trips. The refusal exists because a preValid=false item's
// stored fields were produced by a failure path, not by the parser —
// a fabricated placeholder name (e.g. the non-ANSI-filename item)
// would otherwise be written as a dispatchable line and a resumed
// delete could hit a component that never existed in the batch input.
// Refusing keeps the tail manually resubmittable instead of emitting
// a manifest that retargets. Note the round-trip case is NOT the same
// set: only a preValid=true item reaches the line-emission loop, and
// even degenerate representable fields round-trip — an empty name
// writes "<type>" alone and re-parses to the same "name must not be
// empty" pre-validation failure, a missing source_file to "manifest
// line missing <source_file>", an unknown typeStr to "unknown type" —
// so the resumed batch reproduces the original per-item failure
// instead of dropping it.
// Caller-owned cleanup (§4.1): the file is written once and never deleted
// by the CLI — it must survive for manual resubmission and crash
// forensics. Returns false with `err` set on refusal or I/O failure (an
// I/O failure also removes the partial file, best-effort); on success
// `outPath` carries the absolute path the event line reports.
bool writeResumeManifest(const std::string& dir,
                         const std::vector<BatchItem>& items,
                         size_t firstIndex, bool needsSource,
                         std::string& outPath, std::string& err) {
    std::error_code ec;
    std::filesystem::path base = dir.empty()
        ? std::filesystem::current_path(ec)
        : std::filesystem::path(dir);
    if (ec) {
        err = "failed to resolve the resume manifest directory: " +
              ec.message();
        return false;
    }
    // Representability pre-check BEFORE any file is created (see the
    // comment above): a preValid=false tail item can never become a
    // dispatchable line — refuse rather than write a fabricated name —
    // then line break, leading '"', or '"' combined with whitespace —
    // refuse with the offending item reported (fail-closed, so no
    // corrupt/partial manifest can ever be resubmitted). The caller
    // appends the manual-resume guidance once.
    for (size_t j = firstIndex; j < items.size(); ++j) {
        const BatchItem& it = items[j];
        if (!it.preValid) {
            err = "resume manifest cannot represent item " +
                  std::to_string(j + 1) + " (\"" + it.name +
                  "\"): it already failed manifest pre-validation (" +
                  it.result.errorMessage + ") — no manifest written";
            return false;
        }
        const std::string& n = it.name;
        const bool unrepresentable =
            n.find_first_of(kManifestLineBreaks) != std::string::npos ||
            (!n.empty() && n[0] == '"') ||
            (n.find('"') != std::string::npos &&
             n.find_first_of(kManifestWs) != std::string::npos);
        if (unrepresentable) {
            err = "resume manifest cannot represent item name \"" + n +
                  "\" (a line break, a leading quote, or a quote combined "
                  "with whitespace) — no manifest written";
            return false;
        }
    }

    const std::string fileName =
        "tsdev_resume_" + std::to_string(GetCurrentProcessId()) + "_" +
        std::to_string(GetTickCount64()) + ".manifest";
    const std::filesystem::path path =
        std::filesystem::absolute(base / fileName, ec);
    if (ec) {
        err = "failed to resolve the resume manifest path: " + ec.message();
        return false;
    }
    // path::string() throws (std::system_error) when the resolved path is
    // not representable in the ANSI code page — e.g. a --batch-stdin run
    // from a non-ASCII cwd. Convert ONCE, guarded: an uncaught throw here
    // would escape into the VEH and kill the process mid-batch; a guarded
    // conversion fails closed with an honest "no manifest" diagnostic.
    std::string pathStr;
    try {
        pathStr = path.string();
    } catch (const std::exception&) {
        err = "failed to convert the resume manifest path to a narrow "
              "string (non-ANSI path) — no manifest exists";
        return false;
    }

    std::ostringstream content;
    for (size_t j = firstIndex; j < items.size(); ++j) {
        const BatchItem& it = items[j];
        content << it.typeStr;
        if (!it.name.empty()) {
            // Whitespace in the name (a spaced file stem from a dir
            // batch) requires the quoted-name form parseManifestLines
            // re-reads; '"' and line breaks were refused above.
            const bool needsQuotes =
                it.name.find_first_of(kManifestWs) != std::string::npos;
            content << ' ';
            if (needsQuotes) content << '"';
            content << it.name;
            if (needsQuotes) content << '"';
        }
        if (needsSource && !it.name.empty() && !it.sourceFile.empty()) {
            content << ' ' << it.sourceFile;
        }
        content << '\n';
    }
    const std::string s = content.str();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "failed to write resume manifest \"" + pathStr +
              "\" — no manifest exists";
        return false;
    }
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
    f.close();
    if (!f) {
        // Never leave a truncated manifest behind while reporting that
        // resume is manual — delete the partial file (best-effort).
        std::error_code rmEc;
        std::filesystem::remove(path, rmEc);
        err = "failed to write resume manifest \"" + pathStr +
              "\" — no manifest exists";
        return false;
    }
    outPath = pathStr;
    return true;
}

// Forward declaration: emitBatchOutput is defined below runBatch (same
// anonymous namespace) but the recycle close_failed path must emit the
// results array from the WORKER before TerminateProcess-self — the main
// thread never gets a turn on that path (design §4.1 emission ordering).
void emitBatchOutput(const std::vector<CliResult>& results);

// Run ONE recycle attempt through the engine (design §2 steps 1-6) with the
// production seams: the real census/death sampler and the raw-COM attach.
// `ownSessionExists` is the design-§2 step-2 close-guard conditional — R1
// rule: every production call site passes FALSE, because an attached site
// NEVER invokes the engine (see recycleSiteDefers below). The MyWork dir
// resolves from the lib's discovered MyWork\Bin path; resolution failure
// refuses attach_failed inside the engine (NEVER WHouseEx Create=1 on an
// unresolved path, §2 step 4).
tsdev::RecycleResult runWhserverRecycleCli(TsDevLib* lib,
                                         bool ownSessionExists) {
    tsdev::DispatchMarkerSnapshot marker;
    tsdev::dispatchSnapshot(marker);

    tsdev::WhRecycleSampler sampler;
    tsdev::ComRecycleAttach attach(
        tsdev::resolveMyWorkDirFromBinPath(
            lib ? lib->getMyWorkBinPath() : nullptr));

    tsdev::RecycleParams rp;
    rp.dispatchInFlight  = marker.inFlight;
    rp.ownSessionExists  = ownSessionExists;
    if (ownSessionExists && lib) {
        rp.closeSession = [lib] { return lib->closeWarehouse(); };
    }
    rp.sampler = &sampler;
    rp.attach  = &attach;
    return tsdev::runWhserverRecycle(rp);
}

// --- R1+R2 attached-site defer boundary ---------------------------------------
//
// PROVEN live findings: a process holding an attached warehouse session
// contributes +7 to WHServer's m_nLock for the process's ENTIRE lifetime
// — closeWarehouse does NOT release it (2026-10-20); and ONLY GRACEFUL
// process teardown releases it (2026-10-21): the R1 defer exited via
// TerminateProcess(18) and the dead client's seven references stayed
// orphaned >=5.6 min with no clients alive, so the resumed process's
// census refused census_lock -> exit 19 — the resume chain deadlocked.
// Graceful teardown (closeWarehouse + `delete lib` + ExitProcess) DID
// release the +7 in the same experiment: graceful clients reached
// lock==0 at exit and the server self-reaped ~6 s later.
//
// R1 rule: the engine may run ONLY at a NEVER-ATTACHED site (own
// contribution == 0). R2 rule: at an ATTACHED site — a batch
// between-items breach after any op dispatched, or a serve breach after
// any warehouse-using request — the recycle boundary STILL exits 18
// WITHOUT running the engine, but FIRST runs the §5 close stage itself
// (tsdev::boundedCloseWarehouse — zombie pre-check + monitor-bounded
// closeWarehouse on THIS session-owning thread) so the process can die
// GRACEFULLY: `delete lib` + worker return + normal process teardown —
// the only exit that releases the +7. The caller resumes in a fresh
// process (direct path: the resume-manifest batch; serve: the respawned
// broker) whose own contribution is 0, and the re-encountered breach
// runs the engine there at its never-attached guard site.
//
// The exit-18 shape is the pinned resume contract — the results array +
// marker tail, the resume manifest, and the whserver_recycled event
// carrying the manifest path (the Python client's _resume_manifest_path
// matches only that event name and whserver_recycle_refused; exit_code
// 18 alone drives the serve respawn) — so a GRANTED defer keeps outcome
// Recycled while reason/detail say WHY the recycle was deferred rather
// than performed here. A close that FAILS or outlives the bound is a
// REFUSED defer instead: the §5 close-stage family (close_failed /
// close_timeout / close_bound_invalid / zombie_*_close), exit 19, the
// whserver_recycle_refused event, and the terminateSelf teardown — emit
// + flush + TerminateProcess-self(19), never `delete lib`/ExitProcess
// into a possibly-open warehouse.

// The shared site gate: true => the site is attached and the breach
// defers (no engine run); false => never-attached, the engine may run
// in-process. Both trigger sites use this one rule.
bool recycleSiteDefers(bool ownSessionExists) {
    return ownSessionExists;
}

// The attached-site defer result an attached breach exits on
// (recycleSiteDefers). R2: runs the §5 close stage FIRST — the shared
// boundedCloseWarehouse (zombie pre-check + monitor-bounded close on the
// CALLING/session-owning thread — orlib20 binds CELWHouse per
// (thread,path), so the close must run where the session lives).
//   * close ok  -> GRANTED: Recycled-shaped exit-18 (the resume
//     boundary), safeToDeleteLib — the caller takes the GRACEFUL
//     post-close teardown (`delete lib` + normal teardown), the only
//     exit that releases the +7.
//   * close false / bound fired / zombie gate / bad bound -> REFUSED on
//     the §5 close-stage family: exit 19 + terminateSelf — the own
//     session may still be open.
// onCloseBoundExceeded: EMPTY in production — the helper installs its
// fatal default (stderr + TerminateProcess(19)) since a wedged close
// can never return; tests inject a non-fatal stub that records the fire
// and unblocks the scripted close (the late return is never trusted).
tsdev::RecycleResult recycleDeferredResult(
    const std::function<bool()>& closeSession,
    tsdev::IRecycleSampler* sampler,
    double closeBoundSecs = tsdev::RecycleParams().closeBoundSecs,
    const std::function<void()>& onCloseBoundExceeded = nullptr) {
    std::string cReason, cDetail;
    if (!tsdev::boundedCloseWarehouse(closeSession, sampler,
                                    closeBoundSecs, onCloseBoundExceeded,
                                    &cReason, &cDetail)) {
        tsdev::RecycleResult r;
        r.outcome         = tsdev::RecycleOutcome::RefuseCloseFailed;
        r.exitCode        = 19;
        r.terminateSelf   = true;   // own session may still be open
        r.safeToDeleteLib = false;
        r.reason          = cReason;
        r.detail          = cDetail;
        return r;
    }
    tsdev::RecycleResult r;
    r.outcome         = tsdev::RecycleOutcome::Recycled;
    r.exitCode        = 18;
    r.terminateSelf   = false;  // close succeeded — GRACEFUL teardown
    r.safeToDeleteLib = true;   // releases the +7 (never TerminateProcess)
    r.reason          = "recycle_deferred_own_session";
    r.detail          = "own warehouse session closed; the resumed "
                        "process runs the engine never-attached";
    return r;
}

// WHServer VAS-headroom guard configuration (batch-* and serve). Defaults
// the field-crash analysis: 256 MB total-free headroom and 64 MB for the
// largest contiguous free block (non-LAA 32-bit WHServer dies near ~10 MB
// free; 64 MB of contiguous headroom is the fragmentation tripwire).
// Overridable via --whserver-free-mb / --whserver-largest-free-mb /
// --no-whserver-vas-guard and the TSDEV_WHSERVER_* env mirrors.
struct VasGuardConfig {
    bool     enabled            = true;
    unsigned freeThresholdMB    = 256;
    unsigned largestThresholdMB = 64;
};

// One-shot warning latches for the guard's inconclusive outcomes (one
// emission each per run — a transient failure warns, it never aborts).
// `measure`   = a per-pid measureVasFree failure;
// `whEnum`    = the WHServer pid enumeration itself was inconclusive
//               (a failed Toolhelp32 snapshot is never "no server");
// `crashEnum` = the TSCrashReport enumeration behind the zombie
//               pre-check was inconclusive (never "no zombies").
// runBatch owns one instance for the whole run; runServe keeps one per
// broker lifetime and passes it by reference into serveVasGuardRefuse.
struct VasGuardWarns {
    bool measure   = false;
    bool whEnum    = false;
    bool crashEnum = false;
};

// WHServer gated auto-recycle authorization (design
// tmp/RECYCLE_ENGINE_DESIGN.md §6.1). `authorized` is the OR-ed presence of
// the --recycle-whserver flag and the TSDEV_RECYCLE_WHSERVER env mirror
// (env set-and-not-"0"; there is no negative flag form, so env "0" never
// vetoes an explicit flag). The flag authorizes the recycle engine (R3+)
// — it never bypasses the zero-client census gate.
struct RecycleConfig {
    bool authorized       = false;
    // Source attribution for refusal messages: true when authorization
    // came ONLY from the TSDEV_RECYCLE_WHSERVER env mirror (no explicit
    // --recycle-whserver flag seen), so a refusal can name the env var
    // instead of misattributing the flag.
    bool authorizedViaEnv = false;
};

// The batch-* subcommand set — single source shared by both flag
// validations that restrict to batch subcommands:
// --batch-progress-control (batch-* only) and --recycle-whserver
// (batch-* + serve). The subcommand dispatch in main() encodes the same
// names once more, but that is the command table, not a rule — it stays
// as-is. Pure helper so the -NoTs unit test can pin the set.
bool isBatchSubcommand(const std::string& sub) {
    return iequals(sub, "batch-insert")     ||
           iequals(sub, "batch-modify")     ||
           iequals(sub, "batch-delete")     ||
           iequals(sub, "batch-upsert")     ||
           iequals(sub, "batch-insert-dir") ||
           iequals(sub, "batch-modify-dir") ||
           iequals(sub, "batch-delete-dir") ||
           iequals(sub, "batch-upsert-dir");
}

// §6.1 subcommand restriction: --recycle-whserver is valid only with the
// batch-* subcommands and serve.
bool isRecycleWhserverSubcommand(const std::string& sub) {
    return isBatchSubcommand(sub) || iequals(sub, "serve");
}

// §6.1 validation — run AFTER the elevation guard, same convention as
// --batch-progress-control. Returns true when the combination is legal;
// on a violation fills `r` (exitCode 1 + errorMessage + humanStderr) and
// returns false. Checked in the design's order:
//   1. authorized + VAS guard disabled (--no-whserver-vas-guard /
//      TSDEV_NO_WHSERVER_VAS_GUARD) -> usage error: recycling can never
//      trigger with the guard disabled — a user error, not a silent no-op.
//   2. authorized + --batch-progress-control -> usage error, refused UP
//      FRONT: the stdin control channel (readBatchControlDecision) belongs
//      to the DYING process — a mid-batch recycle exit would strand the
//      open channel and the not-run tail's decision state could not be
//      replayed into the resumed process.
//   3. authorized on a single-op/unknown subcommand -> usage error.
// `authorizedViaEnv` is true when authorization came ONLY from the
// TSDEV_RECYCLE_WHSERVER env mirror: refusals then name the env source
// and the remedy (unset the env var / drop the conflicting flag) instead
// of misattributing an explicit --recycle-whserver flag.
bool validateRecycleWhserver(bool authorized, bool authorizedViaEnv,
                             bool vasGuardEnabled,
                             bool batchControl, const std::string& subcommand,
                             CliResult& r) {
    if (!authorized) return true;
    const std::string envNote =
        authorizedViaEnv
            ? " (authorized via TSDEV_RECYCLE_WHSERVER — unset the env var "
              "or drop the conflicting flag)"
            : "";
    if (!vasGuardEnabled) {
        r.exitCode = 1;
        r.errorMessage = "--recycle-whserver cannot be combined with "
                         "--no-whserver-vas-guard" + envNote;
        r.humanStderr = "tsdev_cli: " + r.errorMessage +
                        " (the guard is the recycle trigger)\n";
        return false;
    }
    if (batchControl) {
        r.exitCode = 1;
        r.errorMessage = "--recycle-whserver cannot be combined with "
                         "--batch-progress-control" + envNote;
        r.humanStderr = "tsdev_cli: " + r.errorMessage +
                        " (the control channel belongs to the dying "
                        "process)\n";
        return false;
    }
    if (!isRecycleWhserverSubcommand(subcommand)) {
        r.exitCode = 1;
        r.errorMessage = "--recycle-whserver is only valid with batch-* "
                         "subcommands and serve" + envNote;
        r.humanStderr = "tsdev_cli: " + r.errorMessage + "\n";
        return false;
    }
    return true;
}

// §6.3 verbatim warning — printed once per process lifetime when the flag
// is in effect; in serve mode once per BROKER lifetime (each respawned
// broker is a new process and re-emits it, which is desired: the warning
// precedes every process that can recycle). Emitted on stderr.
const char kRecycleWhserverWarning[] =
    "tsdev_cli: WARNING --recycle-whserver authorizes restarting WHServer.exe\n"
    "when the VAS guard trips. The restart is gated on a zero-client census\n"
    "(m_nLock==0 sustained >=3s), but a residual race cannot be eliminated:\n"
    "a client holding only a class-factory proxy is invisible to the census\n"
    "and can activate between the census pass and the moment the dying server\n"
    "revokes its COM class at process exit — a measured ~6-7s window\n"
    "(ForceShutDown -> exit 5.9/6.0/6.3s; verify bound 15s) during which the\n"
    "dying server still answers new activations; a client that binds in that\n"
    "window attaches to the DYING server and dies on its NEXT warehouse call\n"
    "(field evidence: TSDev.exe crashed ~6s after server death). On a machine\n"
    "running TS Desktop this can stall or crash the desktop. The gate refuses\n"
    "(exit 19) whenever it cannot confirm zero foreign-held objects — the flag\n"
    "does not bypass the gate. If WHServer cannot be recycled cleanly the batch\n"
    "aborts. In serve mode each recycle respawns the elevated broker — a\n"
    "non-elevated client faces one UAC prompt per respawn (unattended runs\n"
    "cannot satisfy it; run the client elevated or expect the batch to raise).\n"
    "On a recycle exit the batch writes a resume manifest\n"
    "(tsdev_resume_<pid>_<ticks>.manifest beside the source manifest, or the\n"
    "cwd for --batch-stdin) covering the not-run tail; resubmit it with\n"
    "`batch-<op> <resume-file>` (the Python client resubmits it automatically,\n"
    "bounded at 3 recycles per batch; a refusal, exit 19, aborts the batch).\n"
    "The file is caller-owned — the CLI never deletes it.\n";

// Emit the §6.3 warning once per process lifetime (the static guard makes
// the once-per-process/broker-lifetime contract explicit and testable).
void emitRecycleWhserverWarning() {
    static bool warned = false;
    if (warned) return;
    warned = true;
    std::fputs(kRecycleWhserverWarning, stderr);
}

// Presence-semantics env read for the TSDEV_RECYCLE_WHSERVER boolean env
// mirror (same convention as TSDEV_NO_WHSERVER_VAS_GUARD: set to any value
// other than "0" enables; unset/empty/"0" does not).
bool envPresenceEnabled(const char* name) {
    char envBuf[64] = {};
    DWORD n = GetEnvironmentVariableA(name, envBuf, sizeof(envBuf));
    return n > 0 && n < sizeof(envBuf) && !(n == 1 && envBuf[0] == '0');
}

// OR-ed flag/env composition for --recycle-whserver authorization
// (design §6.1): the explicit flag OR TSDEV_RECYCLE_WHSERVER
// set-and-not-"0". The env read lives inside so the whole precedence is
// pinnable in the same TU — env "0" never vetoes an explicit flag (there
// is no negative flag form).
bool resolveRecycleAuthorized(bool flagSeen) {
    return flagSeen || envPresenceEnabled("TSDEV_RECYCLE_WHSERVER");
}

// Strict decimal-MB threshold parse shared by the --whserver-*-mb flags and
// the TSDEV_WHSERVER_*_MB env mirrors. Accepts "0".."1048575" only (fully
// numeric, no sign/space/junk); anything else returns false so the caller
// can reject the flag value or keep the env-default respectively.
bool parseMbThreshold(const std::string& s, unsigned& out) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    const unsigned long v = std::strtoul(s.c_str(), nullptr, 10);
    if (v > 1048575UL) return false;
    out = static_cast<unsigned>(v);
    return true;
}

// Strict decimal-seconds parse shared by the seconds-valued timeout flags
// (--dispatch-timeout-secs, --batch-control-timeout-secs) and the
// TSDEV_DISPATCH_TIMEOUT_SECS env mirror. Accepts "0".."86400" only (fully
// numeric, no sign/space/junk); 0 is the documented disable value. Anything
// else returns false so the caller can reject the flag value or keep the
// env-default respectively.
bool parseTimeoutSecs(const std::string& s, unsigned& out) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    const unsigned long v = std::strtoul(s.c_str(), nullptr, 10);
    if (v > 86400UL) return false;
    out = static_cast<unsigned>(v);
    return true;
}

// Parameters passed to the batch worker thread.
struct BatchParams {
    std::string             op;  // "insert"/"modify"/"delete"
    std::vector<BatchItem>  items;
    const wchar_t*          tsPathOverride;  // nullptr = auto-discover
    bool                    jsonProgress = false;  // --json: NDJSON progress
    bool                    batchControl = false;  // --batch-progress-control
    // Bounded control wait in seconds (R6, design §5): 0 disables the
    // bound (plain blocking read — documented escape hatch); applies only
    // when batchControl is on. Default 900, --batch-control-timeout-secs.
    unsigned                controlTimeoutSecs = 900;
    bool                    pbar = false;   // --pbar: pinned stderr bar
    VasGuardConfig          vasGuard;       // WHServer VAS-headroom guard
    RecycleConfig           recycle;        // --recycle-whserver (R3 engine)
    // Directory the §4.1 resume manifest is written into on a recycle
    // exit (beside the source manifest / the cwd for --batch-stdin / the
    // scanned directory for batch-*-dir; empty -> cwd inside the writer).
    std::string             resumeManifestDir;
    std::vector<CliResult>  results;  // populated by runBatch
    int                     exitCode = 0;   // 0/2/3/14/18/19 (set by worker)
    // Set by the recycle tail-bypass path when it already emitted the JSON
    // results array on the worker (§4.1 order — emission must precede
    // `delete lib` on exit-18 and TerminateProcess-self on close_failed);
    // runBatchAndEmit then skips its own emit — no double-emit.
    bool                    resultsEmitted = false;
};

// The batch worker: constructs ONE TsDevLib, loops over items IN ORDER using
// the shared doInsert/doModify/doDelete logic, collects results, calls
// closeWarehouse() once at the end, then destructs TsDevLib. Continue on
// failure (never abort) — collect all results. Progress is always emitted on
// stderr: one completion line per attempted item (including pre-validation
// and init failures; --json additionally precedes it with a started event)
// plus one batch_completed after the close + result rewrite.
DWORD WINAPI runBatch(LPVOID param) {
    installThreadFatalContainment();  // P4-e: per-thread terminate guard
    BatchParams* p = static_cast<BatchParams*>(param);
    const std::string& op = p->op;
    const size_t total = p->items.size();
    p->results.clear();
    p->results.reserve(total);

    // --pbar: pin the bar at the bottom of stderr BEFORE anything else can
    // write there (the total is known up front, so 0/total draws even when
    // the lib ctor throws). Every progress event then routes through
    // emitLine: under --pbar it scrolls above the pinned bar via
    // eventLine(); otherwise it is byte-identical to the old direct
    // emitBatchProgress calls. --pbar + --json is rejected in main(), so
    // p->jsonProgress is always false on the pbar path and the human event
    // lines render.
    PbarRenderer bar(op, total);
    auto emitLine = [&](const std::string& line) {
        if (p->pbar) bar.eventLine(line);
        else         emitBatchProgress(line);
    };
    if (p->pbar) {
        bar.start();
        // Route tsdev_lib's gated diagnostics through the bar (installed
        // BEFORE `new TsDevLib` so init-path warnings scroll above it too).
        g_pbarLogTarget = &bar;
        tsdev_log_set_sink(&pbarLogSink);
    }

    // Per-item failure detail: under a FAILED marker emit the result's full
    // humanStderr block (compiler errors etc.), minus its trailing CR/LF so
    // the emitter doesn't add a blank line. Empty humanStderr emits nothing
    // — the marker's message= already carries the concise error. Human
    // modes only; --json keeps the strict NDJSON event stream.
    auto emitFailureDetail = [&](const CliResult& r) {
        if (p->jsonProgress || r.success || r.humanStderr.empty()) return;
        std::string detail = r.humanStderr;
        while (!detail.empty() &&
               (detail.back() == '\n' || detail.back() == '\r')) {
            detail.pop_back();
        }
        if (!detail.empty()) emitLine(detail);
    };

    // §7.1 diag: batch start record (doubles as the run header).
    TSDEV_DIAG_LOG("batch.start", "op=%s items=%zu", op.c_str(),
                   p->items.size());

    // Construct ONE TsDevLib instance (all one-time init happens once).
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        // Init failure: every attempted item gets an init_failure result —
        // still an attempted item, so emit the completion line for each
        // (with its started event under --json). With batch control the
        // same per-item handshake applies: a
        // cancel (or a control-channel anomaly) stops before the next item
        // and the remainder become not-run markers. There is no library to
        // close on this path.
        bool cancelled = false;
        size_t index = 0;
        size_t attempted = 0;
        for (auto& item : p->items) {
            ++index;
            if (p->pbar) bar.itemStarted(item.typeStr, item.name);
            // Started events exist only in the --json NDJSON protocol (the
            // client pairs each batch_item_completed with a started).
            if (p->jsonProgress) {
                emitLine(batchItemStartedLine(
                    op, index, total, item.typeStr, item.name,
                    p->jsonProgress));
            }
            CliResult r;
            r.op = op;
            r.typeStr = item.typeStr;
            r.name = item.name;
            r.exitCode = 4;
            r.errorMessage = std::string("init failed: ") + e.what();
            if (p->pbar) {
                if (!r.success) bar.noteFailure();
                bar.itemCompleted();
            }
            emitLine(batchItemCompletedLine(op, index, total, r,
                                            p->jsonProgress));
            emitFailureDetail(r);
            p->results.push_back(std::move(r));
            attempted = index;
            if (p->batchControl && index < total) {
                std::string controlErr;
                const BatchControlDecision decision =
                    readBatchControlDecisionBounded(
                        stdin, p->controlTimeoutSecs, controlErr);
                if (decision == BatchControlDecision::Error) {
                    emitLine(batchControlErrorLine(controlErr,
                                                   p->jsonProgress));
                }
                if (decision != BatchControlDecision::Continue) {
                    cancelled = true;
                    break;
                }
            }
        }
        for (size_t j = attempted; j < total; ++j) {
            p->results.push_back(cancelledBatchItemResult(op, p->items[j]));
        }
        p->exitCode = batchAggregateExitCode(/*batchStarted=*/true,
                                             /*anyFailed=*/true,
                                             cancelled);
        // --pbar: commit the bar as a permanent line first; the summary is
        // the last stderr line underneath it. An init failure is never
        // clean, so the summary always prints.
        if (p->pbar) bar.finish();
        emitBatchProgress(batchCompletedLine(
            op, attempted, total, /*successes=*/0, /*failures=*/attempted,
            /*notRun=*/total - attempted, cancelled, /*closeFailed=*/false,
            p->exitCode, p->jsonProgress));
        tsdev_log_set_sink(nullptr);
        g_pbarLogTarget = nullptr;
        return 0;
    }
    // P4-e: orlib init inside the ctor may have installed its own UEF —
    // re-assert ours outermost before any warehouse call (idempotent).
    installProcessUef();

    // Source-file column in the manifest schema (3-token ops); drives the
    // §4.1 resume-manifest lines on a recycle exit.
    const bool needsSource =
        (op == "insert" || op == "modify" || op == "upsert");
    bool anyFailed = false;
    bool cancelled = false;
    bool aborted = false;            // WHServer VAS-headroom guard fired
    std::string abortReason;         // e.g. "whserver_vas_free=123MB"
    // Breaching-server context for the §4.1 recycle event fields (pid,
    // free_mb) — captured when the guard detects the breach.
    DWORD abortPid = 0;
    uint64_t abortFreeMB = 0;
    // Not-run marker text for the abort arm the guard fired — the VAS
    // default; the zombie arm substitutes its own message.
    const char* abortMarkerMsg = kVasAbortMarkerMessage;
    VasGuardWarns vasWarns;          // one-shot inconclusive warnings
    // Set once any item reaches a real op dispatch (preValid items only —
    // pre-validated failures never touch the warehouse, so they never
    // lazily attach a session). Feeds the R1 recycleSiteDefers gate at
    // the between-items check: a breach after all-pre-validated items is
    // still a never-attached site where the engine may run in-process;
    // a breach after any dispatch is attached and defers (exit 18).
    bool anyOpDispatched = false;
    size_t index = 0;
    size_t attempted = 0;

    // WHServer VAS-headroom guard (TVS-1b/1d): ONE check shared by the
    // pre-flight (below) and the between-items gate (in the loop). Samples
    // WHServer's free VAS; WHServer is a lazy singleton so no pids means
    // nothing to exhaust -> pass. A transient measureVasFree failure warns
    // once per run and does NOT abort. A breach (either threshold) aborts
    // loudly: emit the abort event, set `aborted` + `abortReason` — the
    // shared marker/summary machinery below reports truthfully.
    auto vasGuardCheck = [&]() {
        // snapshotOk overload (P4 g-1): a failed Toolhelp32 walk is
        // INCONCLUSIVE — never "no server". Warn once and pass: the same
        // fail-open protection the collapsing overload gave silently, now
        // honestly reported (never an abort on a failed enumeration).
        std::vector<DWORD> whPids;
        if (!tsdev::findWhServerPids(whPids)) {
            if (!vasWarns.whEnum) {
                vasWarns.whEnum = true;
                emitLine(whserverEnumFailedLine("whserver",
                                                p->jsonProgress));
            }
            TSDEV_DIAG_LOG("whserver.vas.check",
                           "result=whserver_enum_inconclusive");
            return;
        }
        // Corpse pre-check (P4 gap-a): a WHServer pid parenting a
        // TSCrashReport.exe child is a crash-frozen zombie — it still
        // enumerates and its frozen VAS measures healthy under
        // VirtualQueryEx, so the measurement loop below would PASS it and
        // the next dispatch would wedge into the dead session. Abort
        // BEFORE the next dispatch on the same machinery as a VAS breach,
        // honestly labeled whserver_zombie (the exit-14 abort family).
        if (!whPids.empty()) {
            std::vector<DWORD> crashParents;
            if (tsdev::findCrashReportParentPids(crashParents)) {
                const DWORD zombie = whserverZombiePid(whPids, crashParents);
                if (zombie != 0) {
                    TSDEV_DIAG_LOG("whserver.vas.check",
                                   "pid=%lu result=zombie",
                                   static_cast<unsigned long>(zombie));
                    emitLine(whserverZombieAbortLine(zombie,
                                                   p->jsonProgress));
                    aborted = true;
                    abortPid = zombie;
                    abortFreeMB = 0;
                    abortReason = "whserver_zombie";
                    abortMarkerMsg = kZombieAbortMarkerMessage;
                    return;
                }
            } else {
                // Inconclusive — never "no zombies", never an abort on a
                // failed enumeration alone: warn once and proceed (the
                // same fail-open-with-warning convention as measure).
                if (!vasWarns.crashEnum) {
                    vasWarns.crashEnum = true;
                    emitLine(whserverEnumFailedLine("crashreport",
                                                    p->jsonProgress));
                }
                TSDEV_DIAG_LOG("whserver.vas.check",
                               "result=crashreport_enum_inconclusive");
            }
        }
        for (DWORD whPid : whPids) {
            tsdev::WhVasStats vst;
            if (!tsdev::measureVasFree(whPid, vst)) {
                if (!vasWarns.measure) {
                    vasWarns.measure = true;
                    emitLine(whserverVasMeasureFailedLine(
                        whPid, p->jsonProgress));
                }
                TSDEV_DIAG_LOG("whserver.vas.check",
                               "pid=%lu result=measure_failed",
                               static_cast<unsigned long>(whPid));
                continue;
            }
            const uint64_t freeMB =
                vst.totalFreeBytes / (1024 * 1024);
            const uint64_t largestMB =
                vst.largestFreeBlockBytes / (1024 * 1024);
            const bool breach = tsdev::vasBreach(
                freeMB, largestMB, p->vasGuard.freeThresholdMB,
                p->vasGuard.largestThresholdMB);
            TSDEV_DIAG_LOG("whserver.vas.check",
                           "pid=%lu freeMB=%llu largestMB=%llu "
                           "thresholdFreeMB=%u thresholdLargestMB=%u "
                           "decision=%s",
                           static_cast<unsigned long>(whPid),
                           (unsigned long long)freeMB,
                           (unsigned long long)largestMB,
                           p->vasGuard.freeThresholdMB,
                           p->vasGuard.largestThresholdMB,
                           breach ? "abort" : "pass");
            if (breach) {
                emitLine(whserverVasAbortLine(
                    whPid, vst, p->vasGuard.freeThresholdMB,
                    p->vasGuard.largestThresholdMB, p->jsonProgress));
                aborted = true;
                abortPid = whPid;
                abortFreeMB = freeMB;
                // Truthful machine-readable reason for the summary:
                // name the breaching metric + measured MB. Total-free
                // is checked first; when both breach, report it.
                abortReason =
                    (freeMB < p->vasGuard.freeThresholdMB)
                    ? "whserver_vas_free=" + std::to_string(freeMB) + "MB"
                    : "whserver_vas_largest_free=" +
                          std::to_string(largestMB) + "MB";
                return;
            }
        }
    };

    // Recycle exit path (design §2 step-6 tail-bypass): every recycle
    // outcome that ends the batch runs through here instead of the normal
    // tail — including the R2 attached-site DEFER result (recycleDeferredResult,
    // which never ran the engine but DID run the §5 close stage). It
    // appends the recycle not-run markers
    // itself (the tail's marker loop would re-append with the wrong
    // message), SKIPS the tail
    // closeWarehouse + exit-6 rewrite + batchAggregateExitCode (a failed
    // SECOND close could never downgrade 18/19), then runs the §5 teardown:
    // `delete lib` — the identical post-close path a normal batch end runs,
    // safe because step-2 close already succeeded (or no session ever
    // existed; R2: the granted defer's bounded close already ran here) —
    // UNLESS terminateSelf (close_failed, or the REFUSED defer whose own
    // session may still be open): emit + flush, then
    // die via TerminateProcess-self(rec.exitCode), never `delete lib`/ExitProcess
    // into a possibly-open warehouse (§5, dispatch_watchdog convention).
    // A GRANTED defer takes the `delete lib` + return path — the worker
    // joins and main exits 18 GRACEFULLY, the only teardown that releases
    // the +7 (R2, live-proven 2026-10-21).
    // p->exitCode is set directly to 18/19.
    auto recycleExit = [&](const tsdev::RecycleResult& rec) -> DWORD {
        const char* markerMsg =
            (rec.outcome == tsdev::RecycleOutcome::Recycled)
                ? kRecycledMarkerMessage
                : kRecycleRefusedMarkerMessage;
        for (size_t j = attempted; j < total; ++j) {
            p->results.push_back(
                cancelledBatchItemResult(op, p->items[j], markerMsg));
        }
        // §4.1 pinned order: results array -> resume manifest -> event
        // line -> batch_completed summary -> fflush -> exit. The results
        // array is the sole durable record — emit + flush it HERE on the
        // worker BEFORE `delete lib` (whose OrlibExit would otherwise
        // dispatch toward the just-killed/torn server — UNPROVEN shape)
        // and BEFORE any TerminateProcess-self (the main thread never
        // runs again on that path). The flag stops runBatchAndEmit from
        // re-emitting the array.
        if (p->jsonProgress) {
            emitBatchOutput(p->results);
            p->resultsEmitted = true;
        }
        // Step 2 — resume manifest: items from the boundary index onward
        // in the schema parseManifest re-reads, written before the event
        // that carries its path. Caller-owned cleanup: the CLI never
        // deletes it. A write failure is loud (stderr line; the event
        // then carries no manifest member) — the array's marker tail
        // still pins the boundary (§4.4).
        std::string manifestPath;
        if (attempted < total) {
            std::string manifestErr;
            if (!writeResumeManifest(p->resumeManifestDir, p->items,
                                     attempted, needsSource,
                                     manifestPath, manifestErr)) {
                emitLine("tsdev_cli: " + manifestErr +
                         " (resume the remaining items manually)");
            }
            // The event carries the path as JSON text; this is an ANSI
            // (ACP) build, so a non-ASCII path would serialize as U+FFFD
            // and the client would resubmit a nonexistent file. Fail
            // closed: report that auto-resume cannot use the path and
            // emit the event WITHOUT a manifest member (the client then
            // applies its manifest-absent marker text; the file itself
            // still exists on disk for forensics).
            const bool pathIsAscii = std::all_of(
                manifestPath.begin(), manifestPath.end(),
                [](char c) {
                    const unsigned char u = static_cast<unsigned char>(c);
                    return u >= 0x20 && u <= 0x7E;
                });
            if (!manifestPath.empty() && !pathIsAscii) {
                emitLine("tsdev_cli: resume manifest path is not "
                         "representable in the JSON event stream "
                         "(non-ASCII) — automatic resume cannot use it; "
                         "resume the remaining items manually");
                manifestPath.clear();
            }
        }
        // Step 3 — the event AFTER the manifest exists (it carries the
        // path) with the full §4.1 field set: breaching pid, free_mb,
        // the 1-based resume boundary index, total, manifest path.
        emitLine(whserverRecycleEventLine(rec, p->jsonProgress,
                                          abortPid, abortFreeMB,
                                          attempted + 1, total,
                                          manifestPath));
        if (p->pbar) bar.finish();
        // Final summary — the same batch_completed line the normal tail
        // emits, carrying the truthful abort context (the recycle follows
        // a VAS-guard breach). Always printed: a recycle exit is never a
        // clean run.
        size_t successes = 0, failures = 0;
        for (size_t i = 0; i < attempted; ++i) {
            if (p->results[i].success) ++successes; else ++failures;
        }
        emitBatchProgress(batchCompletedLine(
            op, attempted, total, successes, failures,
            /*notRun=*/total - attempted, /*cancelled=*/false,
            /*closeFailed=*/false, rec.exitCode, p->jsonProgress,
            /*aborted=*/true, abortReason));
        tsdev_log_set_sink(nullptr);
        g_pbarLogTarget = nullptr;
        std::fflush(stdout);
        std::fflush(stderr);
        if (rec.terminateSelf) {
            // close_failed: die WITHOUT DLL detach into a possibly-open
            // warehouse (§5, dispatch_watchdog convention).
            TerminateProcess(GetCurrentProcess(),
                             static_cast<UINT>(rec.exitCode));
            std::abort();  // TerminateProcess does not return
        }
        if (rec.safeToDeleteLib) delete lib;
        p->exitCode = rec.exitCode;
        return 0;
    };

    // Pre-flight VAS check (before item 0): the between-items gate only
    // runs after item N completes, so a WHServer already below threshold
    // when the batch starts (the field case — WHServer persisted from a
    // previous session) would leave item 0 unguarded. runBatch has NO
    // explicit warehouse-open step: WHServer.exe is a lazy singleton
    // spawned inside item 0's first orlib20 dispatch, so empty pids here
    // means WHServer is not attached yet — the attach itself spawns a
    // fresh 2 GB process, which is genuinely safe to pass. Only an
    // already-running (depleted) WHServer breaches here.
    if (p->vasGuard.enabled) {
        vasGuardCheck();
    }

    // Recycle trigger — pre-flight site (design §1 row 1): never attached
    // (WHServer lazy-spawns inside item 0), so step-2 close is SKIPPED and
    // a successful recycle lets the batch CONTINUE in-process — item 0's
    // lazy attach is a FIRST activation on a fresh server (the ordinary
    // item-0 path), not the proven-hazardous re-activation-after-close.
    // R1: this is a NEVER-ATTACHED site — the engine may run in-process
    // here (see recycleSiteDefers); a between-items attached-site breach
    // defers via exit 18 and lands back HERE on resume to do the actual
    // recycle.
    if (aborted && p->recycle.authorized) {
        const tsdev::RecycleResult rec =
            runWhserverRecycleCli(lib, /*ownSessionExists=*/false);
        if (rec.outcome == tsdev::RecycleOutcome::Recycled) {
            // Continue-in-process: no boundary/manifest — the event
            // carries only the breach context (pid, free_mb).
            emitLine(whserverRecycleEventLine(rec, p->jsonProgress,
                                              abortPid, abortFreeMB));
            aborted = false;
            abortReason.clear();
        } else {
            return recycleExit(rec);
        }
    }

    for (auto& item : p->items) {
        if (aborted) break;  // pre-flight abort: no item is dispatched
        ++index;
        if (p->pbar) bar.itemStarted(item.typeStr, item.name);
        // Started events exist only in the --json NDJSON protocol (the
        // client pairs each batch_item_completed with a started).
        if (p->jsonProgress) {
            emitLine(batchItemStartedLine(
                op, index, total, item.typeStr, item.name,
                p->jsonProgress));
        }
        // §7.1 diag: per-item start marker (item name + op). Pre-validated
        // failures never reach an op, so they are not marked as started.
        if (item.preValid) {
            TSDEV_DIAG_LOG("batch.item.enter", "op=%s item=%s type=%s",
                           op.c_str(), item.name.c_str(), item.typeStr.c_str());
        }

        CliResult r;
        if (!item.preValid) {
            // Pre-validated failure (malformed line / unreadable file) —
            // carry over the result populated during manifest parsing, just
            // stamp the op.
            r = item.result;
            r.op = op;
        } else {
            // Real dispatch — the first op lazily attaches the warehouse
            // session (WHServer lazy-spawn inside item N's first orlib20
            // call). Mark before dispatch: on a throw the thread dies
            // anyway, and the conservative direction is "session may
            // exist".
            anyOpDispatched = true;
            if (op == "insert") {
                r = doInsert(*lib, item.type, item.typeStr, item.name,
                             item.source, /*flushPerItem=*/false);
            } else if (op == "modify") {
                r = doModify(*lib, item.type, item.typeStr, item.name,
                             item.source);
            } else if (op == "upsert") {
                r = doUpsert(*lib, item.type, item.typeStr, item.name,
                             item.source, /*flushPerItem=*/false);
            } else {  // delete
                r = doDelete(*lib, item.type, item.typeStr, item.name,
                             /*flushPerItem=*/false);
            }
        }

        // §7.1 diag: per-item completion marker — doubles as the
        // partial-batch record (§7.3d) when the process dies mid-run.
        // Guarded like the start marker: only items that reached an op.
        if (item.preValid) {
            TSDEV_DIAG_LOG("batch.item.done",
                           "op=%s item=%s type=%s success=%d exit=%d progId=%d",
                           op.c_str(), item.name.c_str(), item.typeStr.c_str(),
                           r.success ? 1 : 0, r.exitCode, r.progId);
        }

        if (!r.success) anyFailed = true;
        if (p->pbar) {
            if (!r.success) bar.noteFailure();
            bar.itemCompleted();
        }
        // Completion line: under --pbar a successful item emits nothing
        // (the pinned bar is the only output); failures still emit the
        // BATCH FAILED marker plus the full humanStderr detail block.
        if (!p->pbar || !r.success) {
            emitLine(batchItemCompletedLine(op, index, total, r,
                                            p->jsonProgress));
        }
        emitFailureDetail(r);
        p->results.push_back(std::move(r));
        attempted = index;

        // Cooperative control: after each completed non-final item read
        // exactly one decision. "cancel" stops before the next item; a
        // control-channel anomaly emits batch_control_error and also stops.
        // The last item never requests a decision.
        if (p->batchControl && index < total) {
            std::string controlErr;
            const BatchControlDecision decision =
                readBatchControlDecisionBounded(
                    stdin, p->controlTimeoutSecs, controlErr);
            if (decision == BatchControlDecision::Error) {
                emitLine(batchControlErrorLine(controlErr,
                                               p->jsonProgress));
            }
            if (decision != BatchControlDecision::Continue) {
                cancelled = true;
                break;
            }
        }

        // WHServer VAS-headroom guard (TVS-1b): between-items gate — sample
        // AFTER item N completes, BEFORE item N+1 is dispatched (in-flight
        // ops cannot be cancelled). Same shared check as the pre-flight; a
        // breach aborts loudly and breaks the loop.
        if (p->vasGuard.enabled && index < total) {
            vasGuardCheck();
            // Recycle trigger — between-items site (design §1 row 2,
            // R1+R2): a breach here exits 18 + resume through the
            // tail-bypass path either way, but the ENGINE runs
            // in-process only at the never-attached edge — a run of
            // all-pre-validated failures dispatched nothing, so this
            // process holds no WHServer locks and the census can pass.
            // Once any op dispatched the site is ATTACHED: this
            // process's +7 m_nLock contribution lasts its whole
            // lifetime (closeWarehouse cannot release it — only
            // GRACEFUL process teardown does, live-proven 2026-10-21),
            // so an in-process census can never pass and the breach
            // takes the defer boundary instead: R2 runs the §5 close
            // stage here on this session-owning worker thread
            // (recycleDeferredResult -> boundedCloseWarehouse — zombie
            // pre-check + the 30 s monitor bound), then GRACEFULLY
            // exits 18 through `delete lib` + worker return (the only
            // teardown that releases the +7 — the RESUMED process runs
            // the engine at the never-attached pre-flight site). A
            // failed/bounded-out close REFUSES the defer on the §5
            // close-stage family — emit + flush +
            // TerminateProcess-self(19), the open-session teardown
            // class.
            if (aborted && p->recycle.authorized) {
                tsdev::WhRecycleSampler deferSampler;
                const tsdev::RecycleResult rec =
                    recycleSiteDefers(anyOpDispatched)
                        ? recycleDeferredResult(
                              [lib] { return lib->closeWarehouse(); },
                              &deferSampler)
                        : runWhserverRecycleCli(
                              lib, /*ownSessionExists=*/false);
                return recycleExit(rec);
            }
            if (aborted) break;
        }
    }

    // After a cancellation or abort the remaining items are never
    // attempted: append exit-12 not-run markers so the emitted array stays
    // full length. Markers get no item events and no decision reads; the
    // abort marker message says WHY (guard abort vs cooperative cancel).
    for (size_t j = attempted; j < total; ++j) {
        p->results.push_back(cancelledBatchItemResult(
            op, p->items[j],
            aborted ? abortMarkerMsg
                    : "Batch cancelled before this item was attempted"));
    }

    // Flush the warehouse ONCE at the end of the batch (per the TsDevLib
    // header note: "safe to call once at the end of a batch"). A failed
    // explicit close makes every otherwise-successful mutation uncertain, so
    // convert those per-item successes to the existing entry-op failure result
    // and return the existing nonzero aggregate batch status.
    bool closeFailed = false;
    if (!lib->closeWarehouse()) {
        closeFailed = true;
        // emitLine keeps this diagnostic off the pinned bar line under
        // --pbar; without --pbar it writes the identical bytes fprintf did.
        emitLine("tsdev_cli: closeWarehouse failed at end of batch");
        anyFailed = true;
        for (auto& result : p->results) {
            if (!result.success) continue;
            result.success = false;
            result.exitCode = 6;
            result.errorMessage = "closeWarehouse failed at end of batch";
            result.humanStdout.clear();
            result.humanStderr =
                "tsdev_cli: closeWarehouse failed at end of batch\n";
            // --pbar: the rewrite turned an already-counted success into a
            // failure — keep the bar's "— <n> failed" suffix in sync.
            if (p->pbar) bar.noteFailure();
        }
    }

    // Destruct TsDevLib (OrlibExit persists; EDIT control destroyed on this
    // thread) — SKIPPED on close_failed: the failed close leaves the
    // session possibly open with uncertain teardown state, so `delete lib`
    // (OrlibExit) followed by ExitProcess would DLL-detach into it — the
    // §5 close_failed class ("WHouse stays in memory" modal; an invisible
    // deadlock under the SW_HIDE broker). The terminate block below runs
    // the identical tail emission first, so output + exit code are
    // unchanged; only the teardown differs.
    if (!closeFailed) {
        delete lib;
    }

    p->exitCode = batchAggregateExitCode(/*batchStarted=*/true, anyFailed,
                                         cancelled, aborted);

    // §7.1 diag: batch end record (final aggregate exit code).
    TSDEV_DIAG_LOG("batch.end", "op=%s exit=%d", op.c_str(), p->exitCode);

    // Emit the final summary AFTER the close + retroactive rewrite so its
    // counters and exit code reflect the authoritative final results.
    // completed counts attempted items only (markers are reported via
    // not_run); successes/failures count only attempted final outcomes —
    // the close rewrite touches attempted successes only, since markers are
    // already failures.
    size_t successes = 0, failures = 0;
    for (size_t i = 0; i < attempted; ++i) {
        if (p->results[i].success) ++successes; else ++failures;
    }
    // --pbar: commit the bar as a permanent line first; the summary is the
    // last stderr line underneath it. Under --pbar the summary prints only
    // when the batch was NOT fully clean — a clean run's committed bar is
    // the whole report. In the non-pbar modes it always prints.
    if (p->pbar) bar.finish();
    const bool notClean = failures > 0 || cancelled || aborted ||
                          closeFailed || (total - attempted) > 0;
    if (!p->pbar || notClean) {
        emitBatchProgress(batchCompletedLine(
            op, attempted, total, successes, failures,
            /*notRun=*/total - attempted, cancelled, closeFailed,
            p->exitCode, p->jsonProgress, aborted, abortReason));
    }
    tsdev_log_set_sink(nullptr);
    g_pbarLogTarget = nullptr;

    if (closeFailed) {
        // §5 teardown (dispatch_watchdog convention, same as recycleExit):
        // the session may still be open — emit the pending results array
        // ON THIS WORKER (the main thread never runs again; resultsEmitted
        // suppresses runBatchAndEmit's re-emit, the same flag the recycle
        // tail-bypass uses), then flush + TerminateProcess with the
        // aggregate exit code — never `delete lib`/ExitProcess into a
        // possibly-open warehouse. The code is the same value the normal
        // return would have produced (2/3/14 per batchAggregateExitCode).
        if (p->jsonProgress) {
            emitBatchOutput(p->results);
            p->resultsEmitted = true;
        }
        terminateSelfNoDetach(static_cast<unsigned>(p->exitCode));
    }
    return 0;
}

// Serialize a vector of CliResult to a single JSON array on stdout (followed
// by one newline). Reuses the same per-result schema as emitOutput's JSON
// mode (TVS-D.2). stdout contains ONLY the JSON array; the library's stderr
// diagnostic stays on stderr.
void emitBatchOutput(const std::vector<CliResult>& results) {
    ordered_json arr = ordered_json::array();
    for (const CliResult& r : results) {
        arr.push_back(cliResultToJson(r));
    }

    const std::string s = arr.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    std::fwrite(s.c_str(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Shared batch execution + emit helper: sets up BatchParams, launches the
// worker thread (64 MB stack), runs runBatch, emits the JSON array, and
// returns the batch exit code. Used by both cmdBatch() (manifest mode) and
// cmdBatchDir() (directory mode) so the batch execution logic is not
// duplicated.
int runBatchAndEmit(const std::string& op, std::vector<BatchItem>& items,
                    const wchar_t* tsPathOverride, bool jsonProgress,
                    bool batchControl, unsigned controlTimeoutSecs, bool pbar,
                    const VasGuardConfig& vasGuard,
                    const RecycleConfig& recycle,
                    const std::string& resumeManifestDir) {
    // Launch the batch on a 64 MB-stack worker thread (ONE TsDevLib instance).
    BatchParams params;
    params.op = op;
    params.items = std::move(items);
    params.tsPathOverride = tsPathOverride;
    params.jsonProgress = jsonProgress;
    params.batchControl = batchControl;
    params.controlTimeoutSecs = controlTimeoutSecs;
    params.pbar = pbar;
    params.vasGuard = vasGuard;
    params.recycle = recycle;
    params.resumeManifestDir = resumeManifestDir;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runBatch, &params, 0, nullptr);
    if (!hThread) {
        // Thread creation failure -> exit 1 (batch did not run). stderr only.
        std::fprintf(stderr, "tsdev_cli: CreateThread failed: %lu\n",
                     GetLastError());
        return 1;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    // Emit the JSON results array on stdout — only under --json; human and
    // --pbar modes emit no stdout array. Recycle paths already emitted it
    // on the worker (§4.1 order: before `delete lib` / TerminateProcess),
    // so skip here to avoid a double-emit (params.resultsEmitted).
    if (jsonProgress && !params.resultsEmitted) {
        emitBatchOutput(params.results);
    }
    return params.exitCode;
}

// Common batch command handler: parses the manifest, launches the worker,
// emits the JSON array, returns the batch exit code.
int cmdBatch(const std::string& op, const std::string& manifestFile,
             bool useStdin, const wchar_t* tsPathOverride, bool jsonProgress,
             bool batchControl, unsigned controlTimeoutSecs, bool pbar,
             const VasGuardConfig& vasGuard,
             const RecycleConfig& recycle) {
    const bool needsSource = (op == "insert" || op == "modify" || op == "upsert");

    // Parse the manifest on the main thread (before the worker).
    std::vector<BatchItem> items;
    if (useStdin) {
        // Read the manifest from stdin in BINARY mode (avoids CRLF/newline
        // translation on Windows). With --batch-progress-control stdin is
        // the control channel: the manifest is a framed
        // TSDEV_BATCH_CONTROL/1 block and the decision lines after it stay
        // buffered for the worker; without it the whole stream is the
        // manifest, read byte-for-byte to EOF. EOF / empty stdin -> empty
        // manifest.
        _setmode(_fileno(stdin), _O_BINARY);
        std::string content;
        if (batchControl) {
            std::string controlErr;
            if (!readBatchControlManifest(stdin, content, controlErr)) {
                // Pre-run failure: no JSON array, no progress events.
                std::fprintf(stderr, "tsdev_cli: %s\n", controlErr.c_str());
                return 1;
            }
        } else {
            char buf[8192];
            size_t n = 0;
            while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0) {
                content.append(buf, n);
            }
        }
        parseManifestLines(content, needsSource, items);
        if (items.empty()) {
            // No JSON array on stdout (the batch did not run); stderr only.
            std::fprintf(stderr, "tsdev_cli: manifest from stdin is empty\n");
            return 1;
        }
    } else {
        if (batchControl) {
            // The manifest still comes from the file; stdin carries
            // decision lines only (read as binary, like --batch-stdin).
            _setmode(_fileno(stdin), _O_BINARY);
        }
        if (!parseManifest(manifestFile, needsSource, items)) {
            // Manifest file not found / unreadable -> exit 1 (batch did not run).
            // No JSON array on stdout (the batch did not run); stderr only.
            std::fprintf(stderr, "tsdev_cli: failed to open manifest file \"%s\"\n",
                         manifestFile.c_str());
            return 1;
        }
        if (items.empty()) {
            // Manifest contained no real items (only comments/blanks) -> exit 1.
            // No JSON array on stdout (the batch did not run); stderr only.
            std::fprintf(stderr, "tsdev_cli: manifest \"%s\" is empty\n",
                         manifestFile.c_str());
            return 1;
        }
    }

    // Resume-manifest directory (§4.1): beside the source manifest; the
    // cwd for --batch-stdin (no source manifest exists). An unresolvable
    // path leaves it empty -> the writer falls back to the cwd.
    std::string resumeDir;
    {
        std::error_code ec;
        const std::filesystem::path base = useStdin
            ? std::filesystem::current_path(ec)
            : std::filesystem::absolute(
                  std::filesystem::path(manifestFile), ec).parent_path();
        // base.string() throws (std::system_error) on a path the ANSI
        // code page cannot represent (e.g. a non-ASCII cwd under
        // --batch-stdin — the client's only direct batch mode). Catch it:
        // leaving resumeDir empty makes writeResumeManifest fall back to
        // its own guarded cwd conversion, which fails closed with an
        // honest "no manifest" diagnostic — an uncaught throw here would
        // escape into the VEH and TerminateProcess the whole batch.
        if (!ec) {
            try {
                resumeDir = base.string();
            } catch (const std::exception&) {
            }
        }
    }
    return runBatchAndEmit(op, items, tsPathOverride, jsonProgress,
                           batchControl, controlTimeoutSecs, pbar, vasGuard,
                           recycle, resumeDir);
}

// --- TVS-I.2: directory batch mode -------------------------------------------
//
// parseDirectory: enumerate regular files in dirPath using
// std::filesystem::directory_iterator (NOT recursive — subdirectories are
// skipped via is_regular_file()). Filter by accepted extensions: if
// `extensions` is empty, use defaults {".el", ".txt"}; if non-empty, use only
// the specified extensions. Extension matching is case-insensitive (normalize
// by lowercasing both the file extension and the accepted extension). Each
// accepted extension is normalized: prepend '.' if missing and non-empty (so
// "el" -> ".el"; "" stays "" to match extensionless files). For each matching
// file: BatchItem with type = the type int, typeStr = the type string,
// name = file stem (path::stem() strips only the last extension,
// so my.strategy.el -> my.strategy), sourceFile = the file path string,
// source = readFile(path) if needsSource is true, else empty string; a
// successfully read but EMPTY source file fails the item like a read
// failure (preValid=false, exit 3, "source file is empty" — the same
// text parseManifestLines and cmdInsert use). The
// directory_iterator construction is wrapped in try/catch for
// filesystem_error — on error, emit stderr and return false (caller emits
// exit 1 without a second message). The per-entry path::string() narrow
// conversions are likewise guarded: an ACP-unrepresentable filename fails
// THAT item (preValid=false, exit 3) instead of throwing out of the scan.
// Returns true on success (outItems may
// still be empty if no files matched). Items are sorted by name for
// deterministic ordering.
bool parseDirectory(int type, const std::string& typeStr, const std::string& dirPath,
                    const std::vector<std::string>& extensions, bool needsSource,
                    std::vector<BatchItem>& outItems) {
    // Build the normalized accepted-extensions set. If empty, use defaults.
    std::vector<std::string> acceptedExts;
    if (extensions.empty()) {
        acceptedExts = {".el", ".txt"};
    } else {
        for (const std::string& ext : extensions) {
            std::string norm = ext;
            // Prepend '.' if missing and non-empty (so "el" -> ".el"; an
            // empty string stays empty to match extensionless files).
            if (!norm.empty() && norm[0] != '.') norm = "." + norm;
            // Lowercase for case-insensitive comparison.
            std::transform(norm.begin(), norm.end(), norm.begin(),
                [](char c) { return static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c))); });
            acceptedExts.push_back(norm);
        }
    }

    // Enumerate regular files in dirPath (non-recursive).
    std::filesystem::directory_iterator it;
    try {
        it = std::filesystem::directory_iterator(dirPath);
    } catch (const std::filesystem::filesystem_error&) {
        std::fprintf(stderr, "tsdev_cli: failed to open directory \"%s\"\n",
                     dirPath.c_str());
        return false;
    }

    for (const auto& entry : it) {
        // Skip subdirectories — only regular files.
        if (!std::filesystem::is_regular_file(entry.status())) continue;

        const std::filesystem::path& p = entry.path();
        BatchItem item;
        // Narrow-convert extension/stem/full path under ONE guard:
        // path::string() throws std::system_error when a filename is not
        // representable in the ANSI code page. Only the
        // directory_iterator ctor is guarded today — an uncaught throw
        // here would kill the whole batch process (misattributed as an
        // orlib20 crash). Instead fail THAT item loudly: a preValid=false
        // entry is reported as a failure and is never dispatched (no
        // delete can retarget), mirroring the read-failure path below.
        try {
            // Extract the file extension (includes the leading dot;
            // empty if none).
            std::string fileExt = p.extension().string();
            // Lowercase for case-insensitive comparison.
            std::transform(fileExt.begin(), fileExt.end(), fileExt.begin(),
                [](char c) { return static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c))); });

            bool matches = false;
            for (const std::string& acc : acceptedExts) {
                if (fileExt == acc) { matches = true; break; }
            }
            if (!matches) continue;

            item.name = p.stem().string();
            item.sourceFile = p.string();
        } catch (const std::exception&) {
            item.type = static_cast<ComponentType>(type);
            item.typeStr = typeStr;
            // Render the filename identifiably even though it cannot be
            // narrowed: the wide form + UTF-8 survives every character
            // (the JSON emitter replaces anything unrepresentable with
            // U+FFFD — the error_handler_t::replace convention), so the
            // diagnostic names WHICH entry failed instead of surfacing
            // only a placeholder. preValid=false means the name is never
            // dispatched and the resume writer refuses it outright, so
            // it can never reach a manifest line.
            const std::string shown =
                wideToUtf8Cli(p.filename().wstring());
            item.name = "<unrepresentable filename: " + shown + ">";
            item.preValid = false;
            item.result.typeStr = typeStr;
            item.result.name = item.name;
            item.result.exitCode = 3;
            item.result.errorMessage =
                "failed to convert the directory entry \"" + shown +
                "\" path to a narrow string (non-ANSI filename)";
            outItems.push_back(std::move(item));
            continue;
        }
        item.type = static_cast<ComponentType>(type);
        item.typeStr = typeStr;
        if (needsSource) {
            if (!readFile(item.sourceFile, item.source)) {
                // Mirror parseManifestLines: read failure -> preValid false,
                // exit 3, source_file_error. The batch continues and reports
                // this item as a failure.
                item.preValid = false;
                item.result.typeStr = typeStr;
                item.result.name = item.name;
                item.result.exitCode = 3;
                item.result.errorMessage =
                    std::string("failed to read source file \"") + item.sourceFile + "\"";
                outItems.push_back(std::move(item));
                continue;
            }
            // Mirror parseManifestLines and cmdInsert: an EMPTY source
            // file is a pre-validation failure too (same exit-3 item,
            // same message text) — never dispatch empty source.
            if (item.source.empty()) {
                item.preValid = false;
                item.result.typeStr = typeStr;
                item.result.name = item.name;
                item.result.exitCode = 3;
                item.result.errorMessage = "source file is empty";
                outItems.push_back(std::move(item));
                continue;
            }
        }
        item.preValid = true;
        outItems.push_back(std::move(item));
    }

    // Sort by name for deterministic ordering.
    std::sort(outItems.begin(), outItems.end(),
        [](const BatchItem& a, const BatchItem& b) { return a.name < b.name; });
    return true;
}

// cmdBatchDir: directory batch command handler. Validates the type, parses
// the directory into BatchItems via parseDirectory(), then calls the shared
// runBatchAndEmit() helper. If parseDirectory returns false (directory could
// not be opened), the "failed to open directory" message was already emitted
// by parseDirectory — return exit 1 without a second message. If it returns
// true but yielded zero items (empty dir, no matching files), emit the
// "no files matching" message and return exit 1 (no JSON array emitted).
int cmdBatchDir(const std::string& op, int type, const std::string& typeStr,
                const std::string& dirPath, const std::vector<std::string>& extensions,
                const wchar_t* tsPath, bool jsonProgress, bool batchControl,
                unsigned controlTimeoutSecs, bool pbar,
                const VasGuardConfig& vasGuard,
                const RecycleConfig& recycle) {
    // Validate type (unknown -> exit 2, matching existing pattern).
    ComponentType ctype;
    if (!parseType(typeStr, ctype)) {
        std::fprintf(stderr, "tsdev_cli: unknown type \"%s\" "
                     "(expected strategy/function/indicator/tradingapp/showme)\n",
                     typeStr.c_str());
        return 2;
    }

    const bool needsSource = (op == "insert" || op == "modify" || op == "upsert");

    std::vector<BatchItem> items;
    if (!parseDirectory(type, typeStr, dirPath, extensions, needsSource, items)) {
        // Directory open failed — parseDirectory already emitted the
        // "failed to open directory" stderr message. Exit 1, no JSON array.
        return 1;
    }

    if (items.empty()) {
        // No matching files (empty dir or no matches). Exit 1, stderr only,
        // no JSON array emitted. Build the extensions description for the
        // message.
        std::string extDesc;
        if (extensions.empty()) {
            extDesc = ".el,.txt";
        } else {
            for (size_t i = 0; i < extensions.size(); ++i) {
                if (i > 0) extDesc += ",";
                std::string norm = extensions[i];
                if (!norm.empty() && norm[0] != '.') norm = "." + norm;
                extDesc += norm;
            }
        }
        std::fprintf(stderr, "tsdev_cli: no files matching %s found in %s\n",
                     extDesc.c_str(), dirPath.c_str());
        return 1;
    }

    if (batchControl) {
        // Directory enumeration is native; stdin carries decision lines
        // only (read as binary, like --batch-stdin).
        _setmode(_fileno(stdin), _O_BINARY);
    }
    // Resume-manifest directory (§4.1): the scanned directory — the
    // manifest file's ".manifest" extension is outside the default
    // {".el", ".txt"} scan set, so it cannot re-enter a later dir batch.
    return runBatchAndEmit(op, items, tsPath, jsonProgress, batchControl,
                           controlTimeoutSecs, pbar, vasGuard, recycle,
                           dirPath);
}

// --- TVS-K.3: serve request dispatch -----------------------------------------
//
// The serve layer takes a parsed JSON request (from the named-pipe broker in
// TVS-K.4), validates it, and dispatches to the corresponding do* function on
// a shared TsDevLib. This is the pure logic layer with NO pipe I/O — fully
// unit-testable (parseServeRequest is pure; dispatchServeRequest needs a real
// TsDevLib and is validated via E2E in K.6a).

// ServeRequest is the parsed, validated form of a single broker request.
//   op            insert/modify/delete/exists/upsert/get-source/quit/
//                 backup-cache/restore-cache
//   type          ComponentType (resolved from typeStr)
//   typeStr       the original type string verbatim (for CliResult)
//   name          component name
//   source        raw EasyLanguage source TEXT (not a file path — the Python
//                 wrapper reads files client-side and sends the text)
//   caseInsensitive  exists only (optional, default false)
//   outputPath    get-source only (optional; broker-local file path)
//   hasBatch/batchId/batchIndex/batchTotal  optional batch progress tag —
//                 mutating ops only (insert/modify/delete/upsert); hasBatch
//                 gates stderr progress emission and the response "progress"
//                 member (see parseServeRequest/addServeProgress)
//   policy      backup-cache only (optional; verbatim request string —
//                 "skip"|"overwrite"|"abort", absent = default "abort";
//                 "prompt" is rejected at parse: the broker has no console)
struct ServeRequest {
    std::string op;
    ComponentType type = ComponentType::Strategy;
    std::string typeStr;
    std::string name;
    std::string source;
    bool caseInsensitive = false;
    std::string outputPath;
    std::string policy;
    bool hasBatch = false;
    std::string batchId;
    size_t batchIndex = 0;
    size_t batchTotal = 0;
};

// parseServeRequest validates a JSON request and populates `out`. On success
// returns a CliResult with success=true, exitCode=0. On a malformed request
// returns a CliResult with success=false and the appropriate exit code:
//   exit 1 (usage_error): missing 'op', unknown op, missing type/name/source.
//   exit 2 (unknown_type): unknown type string.
// The error messages contain the relevant keyword (op/type/name/source) so
// callers can surface them directly. See the error-handling table in
// docs/workstream_serve_plan.md (lines 530-544).
CliResult parseServeRequest(const ordered_json& req, ServeRequest& out) {
    CliResult r;

    // 1. 'op' is required.
    if (!req.contains("op") || !req["op"].is_string()) {
        r.success = false;
        r.exitCode = 1;
        r.op = "";
        r.errorMessage = "missing 'op' field";
        return r;
    }
    std::string op = req["op"].get<std::string>();

    // 2. Validate the op against the known set.
    const std::string knownOps[] = {
        "insert", "modify", "delete", "exists", "upsert", "get-source",
        "quit", "backup-cache", "restore-cache"
    };
    bool opKnown = false;
    for (const std::string& k : knownOps) {
        if (op == k) { opKnown = true; break; }
    }
    if (!opKnown) {
        r.success = false;
        r.exitCode = 1;
        r.op = "";
        r.errorMessage = "unknown op '" + op + "'";
        return r;
    }

    // Optional 'batch' progress metadata — checked before the 'quit'
    // early-out so quit+batch still rejects. Accepted only on the mutating
    // ops (insert/modify/delete/upsert). When present the object is
    // all-or-none and closed: exactly the members id/index/total — any extra
    // or missing member rejects the whole request. id is a nonempty string;
    // index/total are positive integral JSON numbers that must fit a 32-bit
    // size_t (this binary is built Win32) and satisfy index <= total.
    if (req.contains("batch")) {
        const bool mutating = (op == "insert" || op == "modify" ||
                               op == "delete" || op == "upsert");
        const ordered_json& b = req["batch"];
        // positiveIndex converts a positive integral JSON number to uint64;
        // it rejects bool/null/fraction/string/negative/zero/overflow.
        auto positiveIndex = [](const ordered_json& v, uint64_t& n) -> bool {
            if (v.is_number_unsigned()) {
                n = v.get<uint64_t>();
            } else if (v.is_number_integer()) {
                const int64_t s = v.get<int64_t>();
                if (s < 0) return false;
                n = static_cast<uint64_t>(s);
            } else {
                return false;
            }
            return n >= 1 && n <= 0xFFFFFFFFull;
        };
        uint64_t idx = 0, tot = 0;
        const bool valid = mutating && b.is_object() && b.size() == 3 &&
            b.contains("id") && b["id"].is_string() &&
            !b["id"].get<std::string>().empty() &&
            b.contains("index") && positiveIndex(b["index"], idx) &&
            b.contains("total") && positiveIndex(b["total"], tot) &&
            idx <= tot;
        if (!valid) {
            r.success = false;
            r.exitCode = 1;
            r.op = op;
            r.errorMessage =
                "invalid 'batch' metadata (insert/modify/delete/upsert only; "
                "expected exactly {\"id\":<nonempty string>,"
                "\"index\":<1..total>,\"total\":<positive int>})";
            return r;
        }
        out.hasBatch = true;
        out.batchId = b["id"].get<std::string>();
        out.batchIndex = static_cast<size_t>(idx);
        out.batchTotal = static_cast<size_t>(tot);
    }

    // Optional 'policy' — backup-cache only. Checked before the cache-op
    // early-out so policy on a non-cache op (incl. quit) still rejects. The
    // value must be a string naming skip/overwrite/abort (case-insensitive,
    // like --cache-backup-policy). "prompt" is deliberately rejected: the
    // broker has no console for interactive prompts.
    if (req.contains("policy")) {
        if (op != "backup-cache") {
            r.success = false;
            r.exitCode = 1;
            r.op = op;
            r.errorMessage =
                "'policy' is only valid for op 'backup-cache'";
            return r;
        }
        if (!req["policy"].is_string()) {
            r.success = false;
            r.exitCode = 1;
            r.op = op;
            r.errorMessage =
                "invalid 'policy' (expected a string: 'skip', 'overwrite', "
                "or 'abort')";
            return r;
        }
        const std::string pv = req["policy"].get<std::string>();
        if (!iequals(pv, "skip") && !iequals(pv, "overwrite") &&
            !iequals(pv, "abort")) {
            r.success = false;
            r.exitCode = 1;
            r.op = op;
            r.errorMessage =
                "invalid 'policy' value '" + pv + "' (expected 'skip', "
                "'overwrite', or 'abort'; 'prompt' is unavailable — the "
                "broker has no console)";
            return r;
        }
        out.policy = pv;
    }
    // Absent 'policy' on backup-cache defaults to "abort": the broker has no
    // console to prompt, and abort is the fail-safe default (an existing
    // backup is never silently overwritten or skipped).
    if (op == "backup-cache" && out.policy.empty())
        out.policy = "abort";

    // 3. 'quit' and the cache ops need no type/name/source. The cache ops
    // are pure filesystem — they are not WHServer-mutating, so they bypass
    // the VAS guard and reject 'batch' tags (the batch gate above only
    // accepts insert/modify/delete/upsert).
    if (op == "quit" || op == "backup-cache" || op == "restore-cache") {
        out.op = op;
        r.success = true;
        r.exitCode = 0;
        r.op = op;
        return r;
    }

    // 4. All other ops require 'type' and 'name'.
    if (!req.contains("type") || !req["type"].is_string() ||
        !req.contains("name") || !req["name"].is_string()) {
        r.success = false;
        r.exitCode = 1;
        r.op = op;
        r.errorMessage = "op '" + op + "' requires 'type' and 'name'";
        return r;
    }
    std::string typeStr = req["type"].get<std::string>();
    std::string name = req["name"].get<std::string>();

    // 5. Resolve the type string (case-insensitive, same as CLI).
    ComponentType type;
    if (!parseType(typeStr, type)) {
        r.success = false;
        r.exitCode = 2;
        r.op = op;
        r.typeStr = typeStr;
        r.errorMessage = "unknown type '" + typeStr + "'";
        return r;
    }

    // 6. insert/modify/upsert require 'source' (raw EL text, not a path).
    if (op == "insert" || op == "modify" || op == "upsert") {
        if (!req.contains("source") || !req["source"].is_string()) {
            r.success = false;
            r.exitCode = 1;
            r.op = op;
            r.typeStr = typeStr;
            r.name = name;
            r.errorMessage = "op '" + op + "' requires 'source'";
            return r;
        }
    }

    // 7. Populate the output struct.
    out.op = op;
    out.type = type;
    out.typeStr = typeStr;
    out.name = name;
    if (op == "insert" || op == "modify" || op == "upsert") {
        out.source = req["source"].get<std::string>();
    }
    if (op == "exists") {
        if (req.contains("case_insensitive") && req["case_insensitive"].is_boolean()) {
            out.caseInsensitive = req["case_insensitive"].get<bool>();
        } else {
            out.caseInsensitive = false;
        }
    }
    if (op == "get-source") {
        if (req.contains("output_path") && req["output_path"].is_string()) {
            out.outputPath = req["output_path"].get<std::string>();
        } else {
            out.outputPath.clear();
        }
    }

    r.success = true;
    r.exitCode = 0;
    r.op = op;
    r.typeStr = typeStr;
    r.name = name;
    return r;
}

// addServeProgress enriches the ordinary response frame for a tagged request
// (req.hasBatch): it appends exactly one top-level member
//   "progress":{"batch_id":<id>,"index":<i>,"total":<t>,"phase":"completed"}
// after the existing cliResultToJson members (no envelope, no other top-level
// change). Untagged requests are a no-op — the response stays byte-identical.
void addServeProgress(ordered_json& response, const ServeRequest& req) {
    if (!req.hasBatch) return;
    ordered_json prog;
    prog["batch_id"] = req.batchId;
    prog["index"]    = req.batchIndex;
    prog["total"]    = req.batchTotal;
    prog["phase"]    = "completed";
    response["progress"] = std::move(prog);
}

// taggedParseFailureLines renders the started/completed progress pair for a
// tagged request that failed validation AFTER its 'batch' metadata was
// accepted (e.g. unknown type, missing source): the item still counts as
// attempted, so it emits the same two events as a dispatched item. The
// op/type/name come from the failure CliResult — ServeRequest fields are
// only populated on full parse success. Both strings are empty for untagged
// requests (hasBatch==false — including requests rejected for invalid
// 'batch' metadata itself, which get no progress).
std::pair<std::string, std::string> taggedParseFailureLines(
    const CliResult& r, const ServeRequest& req, bool jsonOutput) {
    if (!req.hasBatch) return {};
    return {batchItemStartedLine(r.op, req.batchIndex, req.batchTotal,
                                 r.typeStr, r.name, jsonOutput),
            batchItemCompletedLine(r.op, req.batchIndex, req.batchTotal, r,
                                   jsonOutput)};
}

// dispatchServeRequest switches on req.op and calls the corresponding do*
// function on the shared TsDevLib. Mutating ops use flushPerItem=false (the
// batch pattern): no per-op closeWarehouse(). The warehouse stays open for
// the entire serve session, and runServe calls closeWarehouse() once at the
// end (before destructing TsDevLib). This is critical because closing the
// warehouse after each op would cause subsequent ops (especially
// exists/get-source which need the warehouse open) to crash with
// ACCESS_VIOLATION in orlib20. Persistence is guaranteed by OrlibExit (in
// the TsDevLib destructor) and the explicit closeWarehouse() in runServe.
// Read-only ops (exists/get-source) need no flush. "quit" returns a success
// CliResult with op="quit" (the broker layer in K.4 handles the actual
// shutdown). "backup-cache"/"restore-cache" are pure filesystem — they take
// no lib state, so they run through runCachePhase's serve path even though
// the session's TsDevLib already exists; the "before any other operation"
// contract is satisfied by sequential request handling (a cache request is
// fully answered before the client's subsequent lib op is dispatched).
//
// NOTE: not unit-tested (needs a real, concrete TsDevLib). Validated via E2E
// in K.6a.
CliResult dispatchServeRequest(TsDevLib& lib, const ServeRequest& req,
                               const wchar_t* tsPathOverride = nullptr) {
    if (req.op == "backup-cache" || req.op == "restore-cache") {
        // Pure-filesystem cache ops: run the same core as the standalone
        // --backup-cache/--restore-cache phase (runCachePhase's serve path).
        // Not WHServer-mutating — they bypass the VAS guard and reject
        // 'batch' tags in parseServeRequest. The policy was validated at
        // parse; empty = default "abort" (the broker never prompts).
        // parseServeRequest already rejected "prompt" and unknown values —
        // that parse-layer rejection is authoritative, so this reparse
        // cannot fail and its return value is intentionally ignored.
        CacheBackupPolicy policy = CacheBackupPolicy::Abort;
        if (!req.policy.empty()) parseCacheBackupPolicy(req.policy, policy);
        CliResult r;
        runCachePhase(req.op == "backup-cache",
                      req.op == "restore-cache", policy, tsPathOverride,
                      /*subcommand=*/"", /*jsonMode=*/false,
                      /*noCacheCompress=*/false, &r);
        // Defensive fallback: unreachable today (runCachePhase/
        // emitCacheErrorJson set r.op on every path), kept so a future
        // cache-phase path that forgets r.op still reports the right op.
        if (r.op.empty()) r.op = req.op;
        return r;
    }
    if (req.op == "insert") {
        return doInsert(lib, req.type, req.typeStr, req.name,
                        req.source, /*flushPerItem=*/false);
    }
    if (req.op == "modify") {
        return doModify(lib, req.type, req.typeStr, req.name, req.source);
    }
    if (req.op == "delete") {
        return doDelete(lib, req.type, req.typeStr, req.name,
                        /*flushPerItem=*/false);
    }
    if (req.op == "upsert") {
        return doUpsert(lib, req.type, req.typeStr, req.name, req.source,
                        /*flushPerItem=*/false);
    }
    if (req.op == "exists") {
        return doExists(lib, req.type, req.typeStr, req.name, req.caseInsensitive);
    }
    if (req.op == "get-source") {
        return doGetSource(lib, req.type, req.typeStr, req.name, req.outputPath);
    }
    if (req.op == "quit") {
        CliResult r;
        r.op = "quit";
        r.success = true;
        r.exitCode = 0;
        return r;
    }

    // Unknown op — should not reach here (parseServeRequest rejects unknown
    // ops), but defend in depth with a usage error.
    CliResult r;
    r.success = false;
    r.exitCode = 1;
    r.errorMessage = "unknown op '" + req.op + "'";
    return r;
}

// --- TVS-K.4: named pipe server ----------------------------------------------
//
// Framing helpers, pipe creation, and the serve loop. The framing layer is
// unit-tested in tests/test_serve_framing.cpp (this .cpp is #included with
// TSDEV_CLI_TEST defined, so the helpers in this anonymous namespace are
// visible to the test). The serve loop + cmdServe are validated via E2E in
// K.6a (they need a real TsDevLib and an elevated broker).

// 64 MiB cap for framed messages (per protocol spec §2). Messages whose
// declared length exceeds this are rejected by readFramed without allocating.
static constexpr size_t kServeFrameCap = 67108864;

// writeAll loops WriteFile until all `len` bytes are written. Returns false on
// error or a zero-byte write (broken pipe). Handles partial writes, which
// named pipes can perform even in PIPE_TYPE_BYTE mode.
bool writeAll(HANDLE h, const void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        DWORD wrote = 0;
        if (!WriteFile(h, static_cast<const char*>(buf) + total,
                       static_cast<DWORD>(len - total), &wrote, nullptr))
            return false;
        if (wrote == 0) return false;  // broken pipe / no progress
        total += wrote;
    }
    return true;
}

// readAll loops ReadFile until all `len` bytes are read. Returns false on
// error or EOF (a zero-byte read before completion indicates a closed pipe).
bool readAll(HANDLE h, void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        DWORD got = 0;
        if (!ReadFile(h, static_cast<char*>(buf) + total,
                      static_cast<DWORD>(len - total), &got, nullptr))
            return false;
        if (got == 0) return false;  // EOF / broken pipe
        total += got;
    }
    return true;
}

// writeFramed writes a 4-byte little-endian length prefix followed by the
// UTF-8 payload. Returns true on success, false on a broken pipe / write
// error (the caller closes the connection).
bool writeFramed(HANDLE pipe, const std::string& payload) {
    uint32_t len = static_cast<uint32_t>(payload.size());
    if (!writeAll(pipe, &len, 4)) return false;
    if (len > 0 && !writeAll(pipe, payload.data(), len)) return false;
    return true;
}

// writeServeResponse writes the ordinary response frame for one request:
// the cliResultToJson body plus the tagged "progress" member (a no-op for
// untagged requests). Exactly one frame per call — used by both the normal
// dispatch path and the parse-failure path.
bool writeServeResponse(HANDLE pipe, const CliResult& r,
                        const ServeRequest& req) {
    ordered_json response = cliResultToJson(r);
    addServeProgress(response, req);
    return writeFramed(pipe, response.dump(-1, ' ', false,
        nlohmann::json::error_handler_t::replace));
}

// readFramed reads a 4-byte little-endian length prefix, rejects lengths
// above the 64 MiB cap, then reads the payload into `out`. Returns true on
// success, false on a broken pipe / EOF / oversized length / short read.
bool readFramed(HANDLE pipe, std::string& out) {
    uint32_t len = 0;
    if (!readAll(pipe, &len, 4)) return false;
    if (len > kServeFrameCap) return false;
    out.resize(len);
    if (len > 0 && !readAll(pipe, out.data(), len)) return false;
    return true;
}

// createServePipe creates the named pipe with the security descriptor from
// protocol spec §3 (Medium-IL + owner/admins/system DACL). Returns the pipe
// handle, or INVALID_HANDLE_VALUE on failure. The pipe is a single-instance,
// duplex, byte-mode, blocking pipe.
HANDLE createServePipe(const std::string& pipeName) {
    // SDDL: O:<userSid> sets the owner to the actual user (not Administrators,
    //   which is the default owner when the broker runs elevated).
    // D:P = protected DACL (no inherited ACEs).
    //   (A;;GA;;;BA)           Administrators: full control.
    //   (A;;GWGR;;;SY)         LOCAL SYSTEM: read/write.
    //   (A;;GWGR;;;<userSid>)  The current user: read/write.
    //   S:(ML;;NW;;;ME)        SACL mandatory label: Medium, No-Write-Up.
    //
    // Why the user's SID instead of the SDDL "OW" (Owner) well-known SID:
    // When the broker runs elevated, the default owner of objects it creates is
    // BUILTIN\Administrators, so "OW" resolves to Administrators. An unelevated
    // client running as the same user has the Administrators group as "deny
    // only" in its token and therefore CANNOT match an OW/Administrators ACE,
    // resulting in ERROR_ACCESS_DENIED (5). Using the user's real SID (and
    // setting it as the owner via O:) makes the ACE match the unelevated client
    // regardless of elevation level.
    std::wstring userSidStr;
    {
        HANDLE hToken = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
            DWORD bufLen = 0;
            GetTokenInformation(hToken, TokenUser, nullptr, 0, &bufLen);
            std::vector<BYTE> buf(bufLen);
            if (bufLen > 0 && GetTokenInformation(hToken, TokenUser, buf.data(),
                                                  bufLen, &bufLen)) {
                const TOKEN_USER* tu =
                    reinterpret_cast<const TOKEN_USER*>(buf.data());
                wchar_t* sidStr = nullptr;
                if (ConvertSidToStringSidW(tu->User.Sid, &sidStr)) {
                    userSidStr = sidStr;
                    LocalFree(sidStr);
                }
            }
            CloseHandle(hToken);
        }
    }

    std::wstring sddl;
    if (userSidStr.empty()) {
        // Fallback: SID retrieval failed — use the old OW-based SDDL.
        // The unelevated client may fail to connect (OW resolves to
        // Administrators when elevated, and unelevated tokens have
        // Administrators as deny-only). Emit a Warning-level diagnostic
        // (visible by default; log levels are runtime-gated) so this is
        // diagnosable if it ever occurs.
        tsdev_log_message(TsDevLogLevel::Warning,
                          "tsdev_cli: WARNING — failed to retrieve user SID "
                          "for pipe security descriptor; falling back to OW-based "
                          "SDDL (unelevated clients may be unable to connect)\n");
        sddl = L"D:P(A;;GA;;;BA)(A;;GWGR;;;SY)(A;;GWGR;;;OW)S:(ML;;NW;;;ME)";
    } else {
        sddl = L"O:" + userSidStr +
               L"D:P(A;;GA;;;BA)(A;;GWGR;;;SY)(A;;GWGR;;;" + userSidStr +
               L")S:(ML;;NW;;;ME)";
    }

    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &pSD, nullptr)) {
        return INVALID_HANDLE_VALUE;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = pSD;

    // Convert the narrow pipe name to wide (UTF-8; pipe names are ASCII here
    // but UTF-8 is safe and forward-compatible).
    int wlen = MultiByteToWideChar(CP_UTF8, 0, pipeName.c_str(),
                                   static_cast<int>(pipeName.size()),
                                   nullptr, 0);
    std::wstring wname(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, pipeName.c_str(),
                        static_cast<int>(pipeName.size()),
                        &wname[0], wlen);

    HANDLE hPipe = CreateNamedPipeW(
        wname.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,   // nMaxInstances (single instance; MVP is single-broker)
        0,   // nOutBufferSize (default)
        0,   // nInBufferSize (default)
        0,   // nDefaultTimeOut (default)
        &sa);

    LocalFree(pSD);  // the SD was copied into the pipe; free our copy
    return hPipe;
}

// Return the existing generic infrastructure status for a ConnectNamedPipe
// attempt. ERROR_PIPE_CONNECTED is the documented normal race where a client
// connected before ConnectNamedPipe was called; every other failed call is
// unexpected and must not fall through as a successful serve session.
int serveConnectExitCode(BOOL connected, DWORD connectError) {
    if (connected || connectError == ERROR_PIPE_CONNECTED) return 0;
    return 1;
}

// TVS-1c: WHServer VAS-headroom guard — serve-path gate. runServe calls this
// once per parsed request BEFORE dispatchServeRequest — the serve-side analog
// of runBatch's between-items gate (serve requests are already per-item: the
// Python client loops items and sends one tagged request each). Only the
// item-mutating ops (insert/modify/delete/upsert) are gated — read-only ops
// (exists/get-source/quit) and parse failures never reach this check.
// WHServer is a lazy singleton: no pids -> pass. A transient measureVasFree
// failure warns once per process lifetime (VasGuardWarns latches, runServe
// scope) and does NOT refuse. A breach (either threshold) emits the same
// batch_aborted event as runBatch and fills `refusal` with an exit-14
// CliResult — error.code "whserver_vas_abort" via errorCodeString, message
// carrying the measured headroom + breaching reason — and returns true; the
// caller writes the refusal response WITHOUT dispatching the op (one request
// still yields exactly one response frame).
// P4 additions: the WHServer pid enumeration uses the snapshotOk overload —
// a failed Toolhelp32 walk is INCONCLUSIVE (never "no server") and warns
// once instead of silently passing (g-1). And a corpse pre-check runs
// BEFORE the VAS measurement (gap-a): a WHServer pid parenting a
// TSCrashReport.exe child is a crash-frozen zombie — still enumerated with
// a healthy-looking frozen VAS, so the measurement would PASS it and the
// next dispatch would wedge into the dead session. A detected zombie
// refuses on the same exit-14 shape (batch_aborted event with
// reason="whserver_zombie", message naming the frozen pid); a failed
// crash-report enumeration warns once and proceeds — never a refusal on
// an inconclusive enumeration.
// R3 recycle wiring + R1/R2 defer rule: when `recycle` is authorized and
// `lib` is provided, a breach (or a detected zombie) produces the
// exit-18/19 recycle-boundary refusal BEFORE the refusal is filled. The
// ENGINE runs in-process only at the never-attached site
// (sessionAttached==false — a first request): an attached broker holds
// +7 m_nLock for the process's whole lifetime (closeWarehouse cannot
// release it — only GRACEFUL process teardown does, live-proven
// 2026-10-21), so an in-process census can never pass; the attached
// breach takes the exit-18 defer boundary instead — R2 runs the §5
// close stage HERE on this session-owning serve-loop thread
// (recycleDeferredResult -> boundedCloseWarehouse), so the defer is
// GRANTED only after a successful bounded close (graceful exit-18 —
// the only teardown that releases the +7) and REFUSED on the §5
// close-stage family (exit 19, terminateSelf) when it is not. The
// client respawns the broker and the resubmitted request reaches this
// same gate at a never-attached site where the engine then runs. The
// refusal carries exit 18/19 and `recycleOut`/`recycleRan` report the
// boundary so runServe can do the §4.3 tail-bypass teardown (respond
// frame -> flush -> delete lib (granted defer: graceful ExitProcess 18)
// or TerminateProcess-self -> broker exits;
// the client respawns and resubmits bounded by max_recycles).
bool serveVasGuardRefuse(const ServeRequest& req,
                         const VasGuardConfig& vasGuard,
                         VasGuardWarns& warns, bool jsonProgress,
                         CliResult& refusal,
                         const RecycleConfig* recycle = nullptr,
                         TsDevLib* lib = nullptr,
                         bool sessionAttached = false,
                         tsdev::RecycleResult* recycleOut = nullptr,
                         bool* recycleRan = nullptr) {
    if (recycleRan) *recycleRan = false;
    if (!vasGuard.enabled) return false;
    const bool mutating = (req.op == "insert" || req.op == "modify" ||
                           req.op == "delete"  || req.op == "upsert");
    if (!mutating) return false;

    // Shared refusal tail for a tripped guard (breach or corpse): with
    // recycle authorized the recycle boundary is produced FIRST and its
    // outcome becomes the response's exit code (18/19 — design §4.3: the
    // frame carries error.code whserver_recycled / whserver_recycle_refused
    // via errorCodeString, and the caller does the tail-bypass teardown);
    // without it the request is refused on the plain exit-14 guard class —
    // the request is never consumed (no dispatch, no item events).
    // R1/R2: the boundary is produced by the engine only at the
    // never-attached site; an attached broker defers — R2 runs the §5
    // close stage inside recycleDeferredResult BEFORE this refusal is
    // filled, so the frame + event below ALWAYS carry the truthful
    // outcome (granted defer 18 / close-refused 19 — the client never
    // reads a stale "recycled" from a broker whose close failed).
    auto refuseGuard = [&](const std::string& reason,
                           const std::string& plainMessage) {
        if (recycle && recycle->authorized && lib) {
            const bool deferred = recycleSiteDefers(sessionAttached);
            tsdev::RecycleResult rec;
            if (deferred) {
                tsdev::WhRecycleSampler deferSampler;
                rec = recycleDeferredResult(
                    [lib] { return lib->closeWarehouse(); },
                    &deferSampler);
            } else {
                rec = runWhserverRecycleCli(
                    lib, /*ownSessionExists=*/false);
            }
            emitBatchProgress(whserverRecycleEventLine(rec, jsonProgress));
            if (recycleOut) *recycleOut = rec;
            if (recycleRan) *recycleRan = true;
            refusal = CliResult();
            refusal.op = req.op;
            refusal.typeStr = req.typeStr;
            refusal.name = req.name;
            refusal.success = false;
            refusal.exitCode = rec.exitCode;
            if (rec.outcome == tsdev::RecycleOutcome::Recycled) {
                refusal.errorMessage =
                    deferred
                        ? "WHServer recycle deferred (" + reason +
                          "); resubmit on the respawned broker"
                        : "WHServer recycled (" + reason +
                          "); resubmit on the respawned broker";
            } else {
                refusal.errorMessage =
                    "WHServer recycle refused: " + rec.reason +
                    (rec.detail.empty() ? "" : " " + rec.detail) +
                    " (" + reason + "); refusing " + req.op +
                    " before dispatch";
            }
            return;
        }
        refusal = CliResult();
        refusal.op = req.op;
        refusal.typeStr = req.typeStr;
        refusal.name = req.name;
        refusal.success = false;
        refusal.exitCode = 14;
        refusal.errorMessage = plainMessage;
    };

    // snapshotOk overload (P4 g-1): a failed Toolhelp32 walk is
    // INCONCLUSIVE — never "no server". Warn once and pass: the same
    // fail-open protection the collapsing overload gave silently, now
    // honestly reported (never a refusal on a failed enumeration).
    std::vector<DWORD> whPids;
    if (!tsdev::findWhServerPids(whPids)) {
        if (!warns.whEnum) {
            warns.whEnum = true;
            emitBatchProgress(whserverEnumFailedLine("whserver",
                                                   jsonProgress));
        }
        TSDEV_DIAG_LOG("whserver.vas.check",
                       "result=whserver_enum_inconclusive");
        return false;
    }
    // Corpse pre-check (P4 gap-a): a WHServer pid parenting a
    // TSCrashReport.exe child is a crash-frozen zombie — it still
    // enumerates and its frozen VAS measures healthy under VirtualQueryEx,
    // so the loop below would PASS it and the next dispatch would wedge
    // into the dead session. Refuse BEFORE the dispatch.
    if (!whPids.empty()) {
        std::vector<DWORD> crashParents;
        if (tsdev::findCrashReportParentPids(crashParents)) {
            const DWORD zombie = whserverZombiePid(whPids, crashParents);
            if (zombie != 0) {
                TSDEV_DIAG_LOG("whserver.vas.check",
                               "pid=%lu result=zombie",
                               static_cast<unsigned long>(zombie));
                emitBatchProgress(whserverZombieAbortLine(zombie,
                                                        jsonProgress));
                refuseGuard("whserver_zombie",
                            whserverZombieRefusalMessage(req.op, zombie));
                return true;
            }
        } else {
            // Inconclusive — never "no zombies", never a refusal on a
            // failed enumeration alone: warn once and proceed (the same
            // fail-open-with-warning convention as measure).
            if (!warns.crashEnum) {
                warns.crashEnum = true;
                emitBatchProgress(whserverEnumFailedLine("crashreport",
                                                       jsonProgress));
            }
            TSDEV_DIAG_LOG("whserver.vas.check",
                           "result=crashreport_enum_inconclusive");
        }
    }
    for (DWORD whPid : whPids) {
        tsdev::WhVasStats vst;
        if (!tsdev::measureVasFree(whPid, vst)) {
            if (!warns.measure) {
                warns.measure = true;
                emitBatchProgress(whserverVasMeasureFailedLine(
                    whPid, jsonProgress));
            }
            TSDEV_DIAG_LOG("whserver.vas.check",
                           "pid=%lu result=measure_failed",
                           static_cast<unsigned long>(whPid));
            continue;
        }
        const uint64_t freeMB    = vst.totalFreeBytes        / (1024 * 1024);
        const uint64_t largestMB = vst.largestFreeBlockBytes / (1024 * 1024);
        const bool breach = tsdev::vasBreach(freeMB, largestMB,
                                             vasGuard.freeThresholdMB,
                                             vasGuard.largestThresholdMB);
        TSDEV_DIAG_LOG("whserver.vas.check",
                       "pid=%lu freeMB=%llu largestMB=%llu "
                       "thresholdFreeMB=%u thresholdLargestMB=%u decision=%s",
                       static_cast<unsigned long>(whPid),
                       (unsigned long long)freeMB,
                       (unsigned long long)largestMB,
                       vasGuard.freeThresholdMB,
                       vasGuard.largestThresholdMB,
                       breach ? "refuse" : "pass");
        if (breach) {
            emitBatchProgress(whserverVasAbortLine(
                whPid, vst, vasGuard.freeThresholdMB,
                vasGuard.largestThresholdMB, jsonProgress));
            // Same truthfulness rule as runBatch: name the breaching metric
            // + measured MB (total-free is checked first; when both breach,
            // report it).
            const std::string reason =
                (freeMB < vasGuard.freeThresholdMB)
                ? "whserver_vas_free=" + std::to_string(freeMB) + "MB"
                : "whserver_vas_largest_free=" +
                      std::to_string(largestMB) + "MB";
            refuseGuard(reason,
                        "WHServer VAS headroom below threshold: free=" +
                        std::to_string(freeMB) + "MB largest=" +
                        std::to_string(largestMB) + "MB (" + reason +
                        "); refusing " + req.op + " before dispatch");
            return true;
        }
    }
    return false;
}

// Parameters passed to the serve worker thread.
struct ServeParams {
    HANDLE             pipe = INVALID_HANDLE_VALUE;
    const wchar_t*     tsPathOverride = nullptr;  // nullptr = auto-discover
    bool               jsonProgress = false;      // --json: NDJSON progress
    VasGuardConfig     vasGuard;                  // WHServer VAS-headroom guard
    RecycleConfig      recycle;                   // --recycle-whserver (R3 engine)
    std::atomic<bool>  quit{false};               // set by worker on 'quit' op
    int                exitCode = 0;              // set by worker
};

// runServe is the 64MB-stack worker thread entry point. It constructs ONE
// TsDevLib (lives for the whole serve session, like runBatch), then loops:
// ConnectNamedPipe -> inner loop (readFramed -> parseServeRequest ->
// dispatchServeRequest -> cliResultToJson -> writeFramed) ->
// FlushFileBuffers + DisconnectNamedPipe. The 'quit' op sets the quit flag
// and breaks both loops. TsDevLib is constructed AND used on this same
// thread (thread-affine constraint: the hidden EDIT control is thread-local).
DWORD WINAPI runServe(LPVOID param) {
    installThreadFatalContainment();  // P4-e: per-thread terminate guard
    ServeParams* p = static_cast<ServeParams*>(param);
    HANDLE pipe = p->pipe;
    VasGuardWarns vasWarns;         // one-shot inconclusive-warning
                                    // latches (once per process lifetime)
    bool sessionAttached = false;   // set once a WAREHOUSE-USING request
                                    // dispatched (or threw mid-dispatch,
                                    // conservatively) — feeds the R1
                                    // recycleSiteDefers gate (attached
                                    // => defer exit 18; never-attached
                                    // => engine may run in-process)

    // Construct ONE TsDevLib instance (all one-time init happens once).
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "tsdev_cli: serve init failed: %s\n", e.what());
        p->exitCode = 4;
        return 0;
    }
    // P4-e: orlib init inside the ctor may have installed its own UEF —
    // re-assert ours outermost before any warehouse call (idempotent).
    installProcessUef();

    // Outer loop: accept connections until the 'quit' op (or a fatal
    // ConnectNamedPipe failure).
    while (!p->quit.load()) {
        // §7.1 diag (idle-phase marker): mark the pipe wait as an idle-by-design
        // phase. ConnectNamedPipe blocks here for as long as no client
        // connects — healthy for the long-lived serve broker — so the
        // watchdog emits a periodic idle-heartbeat progress record instead
        // of counting this wait toward the fire threshold (the 2026-09-02
        // smoke run was a healthy idle `serve` aborted by the watchdog).
        // Observation-only markers; no-ops in the production build.
        TSDEV_DIAG_IDLE_PHASE_BEGIN("serve.pipe-wait");
        BOOL connected = ConnectNamedPipe(pipe, nullptr);
        TSDEV_DIAG_IDLE_PHASE_END();
        DWORD connectError = connected ? ERROR_SUCCESS : GetLastError();
        int connectExitCode = serveConnectExitCode(connected, connectError);
        if (connectExitCode != 0) {
            // ConnectNamedPipe failed unexpectedly — stop the broker with a
            // nonzero infrastructure result instead of falling through as a
            // successful serve session. ERROR_PIPE_CONNECTED is handled by
            // serveConnectExitCode as the normal client-connect race.
            p->exitCode = connectExitCode;
            std::fprintf(stderr,
                "tsdev_cli: ConnectNamedPipe failed (err=%lu)\n",
                connectError);
            break;
        }

        // Inner loop: read framed requests, dispatch, write framed responses.
        while (!p->quit.load()) {
            std::string reqStr;
            if (!readFramed(pipe, reqStr)) {
                break;  // broken pipe / EOF / oversized -> close connection
            }

            try {
            // Parse the JSON request. Malformed JSON -> usage error response.
            CliResult r;
            ordered_json reqJson;
            try {
                reqJson = ordered_json::parse(reqStr);
            } catch (const std::exception&) {
                r.success = false;
                r.exitCode = 1;
                r.errorMessage = "malformed JSON request";
                if (!writeFramed(pipe, cliResultToJson(r).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace))) break;
                continue;
            }

            // Validate + parse the request into a ServeRequest.
            ServeRequest req;
            r = parseServeRequest(reqJson, req);
            if (!r.success) {
                // A request whose 'batch' metadata validated still counts
                // as an attempted batch item even when a later validation
                // step fails (unknown type, missing source, ...): emit the
                // same started/completed pair and enrich the ordinary
                // failure response — still exactly one frame. Requests
                // rejected for invalid 'batch' metadata itself keep
                // hasBatch==false, so the pair is empty (untagged failure,
                // no progress).
                const auto lines =
                    taggedParseFailureLines(r, req, p->jsonProgress);
                if (!lines.first.empty())  emitBatchProgress(lines.first);
                if (!lines.second.empty()) emitBatchProgress(lines.second);
                if (!writeServeResponse(pipe, r, req)) break;
                continue;
            }

            // 'quit' op: respond with the quit success result, then signal
            // shutdown and break the inner loop.
            if (req.op == "quit") {
                CliResult qr = dispatchServeRequest(*lib, req,
                                                    p->tsPathOverride);
                writeFramed(pipe, cliResultToJson(qr).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
                p->quit.store(true);
                break;
            }

            // WHServer VAS-headroom guard (TVS-1c): gate BEFORE dispatch,
            // mirroring runBatch's between-items check — a tagged request
            // that is refused was never attempted, so it gets NO
            // started/completed item events (like a batch not-run marker);
            // the refusal response frame still carries the tagged
            // "progress" member via writeServeResponse.
            CliResult vasRefusal;
            tsdev::RecycleResult recycleOutcome;
            bool recycleRan = false;
            if (serveVasGuardRefuse(req, p->vasGuard, vasWarns,
                                    p->jsonProgress, vasRefusal,
                                    &p->recycle, lib, sessionAttached,
                                    &recycleOutcome, &recycleRan)) {
                // Best-effort write (design §4.3 teardown rule): when the recycle ran the broker
                // MUST tear down regardless of the write result — a broker
                // left alive after close/skip-close is an open-warehouse
                // linger / 0x4E re-activation hazard.
                const bool refusalWritten =
                    writeServeResponse(pipe, vasRefusal, req);
                if (recycleRan) {
                    // §4.3/§5 tail-bypass teardown: flush the pipe, then
                    // exit the broker — delete lib (post-close teardown)
                    // where safe, or emit+flush+TerminateProcess-self(19)
                    // on the close_failed path. The tail closeWarehouse +
                    // exit-1 downgrade is never reached; the process exit
                    // code is 18/19 directly.
                    // R2 granted defer: terminateSelf==false +
                    // safeToDeleteLib==true — the bounded close already
                    // ran inside serveVasGuardRefuse (above), so this
                    // takes the GRACEFUL `delete lib` + worker-return +
                    // ExitProcess(18) path — the only teardown that
                    // releases the broker's +7 m_nLock contribution (a
                    // TerminateProcess corpse orphans it for >=minutes,
                    // live-proven 2026-10-21). A REFUSED defer
                    // (terminateSelf) dies the open-session way — never
                    // `delete lib`/ExitProcess into a possibly-open
                    // warehouse.
                    FlushFileBuffers(pipe);
                    if (recycleOutcome.terminateSelf) {
                        std::fflush(stdout);
                        std::fflush(stderr);
                        TerminateProcess(GetCurrentProcess(),
                            static_cast<UINT>(recycleOutcome.exitCode));
                        std::abort();  // TerminateProcess does not return
                    }
                    if (recycleOutcome.safeToDeleteLib) delete lib;
                    p->exitCode = recycleOutcome.exitCode;
                    return 0;
                }
                if (!refusalWritten) break;
                continue;
            }

            // Batch-tagged requests emit the same item progress to the serve
            // stderr (human or NDJSON per --json), flushed per event.
            if (req.hasBatch) {
                emitBatchProgress(batchItemStartedLine(
                    req.op, req.batchIndex, req.batchTotal, req.typeStr,
                    req.name, p->jsonProgress));
            }

            // Dispatch on THIS thread (TsDevLib is thread-affine to this
            // 64MB-stack worker). Catch unexpected exceptions and respond
            // with an internal_error result (exit 1) instead of crashing.
            // NOTE: do NOT use exit 6 here — exit 6 is only used for
            // entry_op_failure (insert/modify/delete/upsert), so an unexpected
            // exception would be misclassified. Exit 1 / internal_error is
            // unambiguous.
            // Warehouse-using ops only (the R1 recycleSiteDefers gate):
            // backup-cache/restore-cache are pure filesystem and never
            // attach — they must NOT set sessionAttached (a breach would
            // then defer at a site that is really never-attached — a
            // spurious extra respawn). Every other dispatched op
            // attaches lazily on first use ('quit' is handled above and
            // never reaches here).
            const bool warehouseOp =
                (req.op != "backup-cache" && req.op != "restore-cache");
            try {
                r = dispatchServeRequest(*lib, req, p->tsPathOverride);
                // The first dispatched warehouse request lazily attaches
                // the session — from here on a recycle breach is an
                // attached site and defers (R1 recycleSiteDefers).
                if (warehouseOp) sessionAttached = true;
            } catch (const std::exception& e) {
                // Conservative-on-throw: a warehouse op may have
                // attached before throwing — treat the session as
                // existing so a later breach defers rather than running
                // the engine at a misclassified never-attached site.
                if (warehouseOp) sessionAttached = true;
                r = CliResult();
                r.op = req.op;
                r.typeStr = req.typeStr;
                r.name = req.name;
                r.success = false;
                r.exitCode = 1;
                r.errorMessage = std::string("internal error: ") + e.what();
            }
            if (req.hasBatch) {
                emitBatchProgress(batchItemCompletedLine(
                    req.op, req.batchIndex, req.batchTotal, r,
                    p->jsonProgress));
            }
            // One request still yields exactly one response frame — the
            // ordinary cliResultToJson body plus the tagged "progress"
            // member (a no-op for untagged requests).
            if (!writeServeResponse(pipe, r, req)) break;
            } catch (...) {
                // Belt-and-suspenders: if any exception escapes the inner
                // processing (including dump() serialization), write a JSON
                // error frame to the pipe instead of crashing (which would
                // launch TSCrashReport.exe via the unhandled-exception filter).
                const char* errFrame =
                    "{\"op\":\"unknown\",\"success\":false,\"exit_code\":1,"
                    "\"error\":{\"code\":\"internal_error\","
                    "\"message\":\"internal error during output serialization\"}}";
                writeFramed(pipe, std::string(errFrame));
                continue;
            }
        }

        // Flush any buffered output, then disconnect so the next client can
        // connect (the pipe handle is reused across connections).
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
    }

    // Flush the warehouse once at the end of the serve session, then destruct
    // TsDevLib (OrlibExit persists; EDIT control destroyed on this thread).
    // A failed explicit close is an infrastructure failure; preserve any
    // earlier nonzero status (for example, an unexpected ConnectNamedPipe
    // failure) and never let the session fall through to exit 0.
    if (!lib->closeWarehouse()) {
        std::fprintf(stderr,
            "tsdev_cli: closeWarehouse failed at end of serve session\n");
        if (p->exitCode == 0) p->exitCode = 1;
        // §5 teardown (dispatch_watchdog convention): the failed close
        // leaves the session possibly open with uncertain teardown state —
        // `delete lib` (OrlibExit) + ExitProcess would DLL-detach into it,
        // raising the blocking "WHouse stays in memory" dialog — an
        // INVISIBLE deadlock under this SW_HIDE broker. All pipe output
        // was already flushed at the disconnect above and the diagnostic
        // went to stderr, so die here via TerminateProcess with the same
        // exit code the return below would have produced.
        terminateSelfNoDetach(static_cast<unsigned>(p->exitCode));
    }
    delete lib;
    return 0;
}

// cmdServe is the 'serve' subcommand handler. It parses --serve-pipe-file
// (optional), creates the named pipe with the security descriptor, writes
// the pipe name + PID to the pipe file (atomic: .tmp + MoveFileEx) or prints
// the pipe name to stdout, launches runServe on a 64MB-stack thread, and
// waits for it to exit.
int cmdServe(int argc, char** argv, bool jsonMode,
             const wchar_t* tsPathOverride,
             const VasGuardConfig& vasGuard,
             const RecycleConfig& recycle) {
    // Parse --serve-pipe-file <path> (optional; survives the global pre-scan
    // because it is serve-specific, like --case-insensitive/--output).
    std::string pipeFile;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--serve-pipe-file") {
            if (i + 1 >= argc) {
                std::fprintf(stderr,
                    "tsdev_cli: --serve-pipe-file requires a file path\n");
                return 1;
            }
            pipeFile = argv[i + 1];
            i++;  // skip the value
        } else if (a.rfind("--serve-pipe-file=", 0) == 0) {
            pipeFile = a.substr(std::strlen("--serve-pipe-file="));
            if (pipeFile.empty()) {
                std::fprintf(stderr,
                    "tsdev_cli: --serve-pipe-file requires a non-empty path\n");
                return 1;
            }
        } else {
            std::fprintf(stderr,
                "tsdev_cli: unknown serve argument \"%s\"\n", a.c_str());
            return 1;
        }
    }

    // Build the pipe name: \\.\pipe\tsdev_cli_<PID> (PID for uniqueness).
    DWORD pid = GetCurrentProcessId();
    std::string pipeName = "\\\\.\\pipe\\tsdev_cli_" + std::to_string(pid);

    HANDLE hPipe = createServePipe(pipeName);
    if (hPipe == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr,
            "tsdev_cli: failed to create named pipe \"%s\" (err=%lu)\n",
            pipeName.c_str(), GetLastError());
        return 1;
    }

    // Publish the pipe name. With --serve-pipe-file: write the full pipe name
    // (line 1) and the broker PID (line 2) atomically (write to <path>.tmp,
    // then MoveFileEx with REPLACE_EXISTING). Without it: print the pipe name
    // to stdout for manual use.
    if (!pipeFile.empty()) {
        std::string content = pipeName + "\n" + std::to_string(pid) + "\n";
        std::string tmp = pipeFile + ".tmp";
        if (!writeFile(tmp, content) ||
            !MoveFileExA(tmp.c_str(), pipeFile.c_str(),
                         MOVEFILE_REPLACE_EXISTING)) {
            std::fprintf(stderr,
                "tsdev_cli: failed to write pipe file \"%s\" (err=%lu)\n",
                pipeFile.c_str(), GetLastError());
            CloseHandle(hPipe);
            return 1;
        }
    } else {
        std::fprintf(stdout, "%s\n", pipeName.c_str());
        std::fflush(stdout);
    }

    // Launch runServe on a 64MB-stack worker thread (ONE TsDevLib instance,
    // constructed and used on that thread).
    ServeParams params;
    params.pipe = hPipe;
    params.tsPathOverride = tsPathOverride;
    params.jsonProgress = jsonMode;
    params.vasGuard = vasGuard;
    params.recycle = recycle;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runServe, &params, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "tsdev_cli: CreateThread failed: %lu\n",
                     GetLastError());
        CloseHandle(hPipe);
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);
    CloseHandle(hPipe);
    return params.exitCode;
}

// Vectored Exception Handler — catches fatal exceptions (access violations,
// stack overflows, heap corruption) BEFORE orlib20's unhandled exception
// filter can spawn TSCrashReport.exe. Calls TerminateProcess with the
// exception code so the process exits cleanly with a known exit code
// and NO crash dialog. This is critical for serve mode: when the elevated
// broker crashes, TSCrashReport.exe would run elevated and a non-elevated
// Python client cannot kill it.
LONG WINAPI vehCrashHandler(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Only handle truly fatal, noncontinuable exceptions — never normal
    // C++ exceptions (0xE06D7363 — continuable until proven unhandled) or
    // debug breakpoints (a VEH eat would kill the process instead of
    // breaking into the debugger). Unhandled codes OUTSIDE this list still
    // die with the SEH code one dispatch later in uefCrashHandler; this
    // list exists to die one dispatch earlier and to cover any window in
    // which orlib20's UEF is the outermost filter (P4 gap-e ordering).
    // Conservative list — every entry is fatal-by-default, never a
    // continuable/benign trap: VEH sees EVERY exception in the process, so
    // adding a benign code here would kill on first occurrence.
    if (code == EXCEPTION_ACCESS_VIOLATION ||       // 0xC0000005
        code == EXCEPTION_STACK_OVERFLOW ||          // 0xC00000FD
        code == STATUS_STACK_BUFFER_OVERRUN ||       // 0xC0000409
        code == 0xC0000374 ||                        // STATUS_HEAP_CORRUPTION (ntstatus.h not included due to winnt.h conflicts)
        code == EXCEPTION_ILLEGAL_INSTRUCTION ||     // 0xC000001D
        code == EXCEPTION_PRIV_INSTRUCTION ||        // 0xC0000096
        code == EXCEPTION_IN_PAGE_ERROR) {           // 0xC0000006
        TerminateProcess(GetCurrentProcess(), code);
        // TerminateProcess doesn't return, but just in case:
        return EXCEPTION_EXECUTE_HANDLER;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// Own-process fatal-termination containment (P4 gap-e hardening)
//
// The VEH above converts the most common fatal SEH codes into a
// TerminateProcess(code) silent death — no dialog, no WerFault, no
// TSCrashReport.exe. Field evidence (2026-10-19): tsdev_cli.exe crashed 4x
// WITH TSCrashReport — a fatal path bypassed the VEH. The remaining fatal
// channels of OUR process, and the mechanism that closes each:
//
//   1. C++ exceptions that would kill the process — two delivery paths,
//      both closed. (a) std::terminate itself — noexcept violations,
//      exceptions during unwinding, frames that filter to terminate — is
//      closed by std::set_terminate -> onFatalTerminate ->
//      TerminateProcess(20). set_terminate is PER-THREAD in the MSVC CRT
//      (MSDN: "terminate functions are maintained separately for each
//      thread; each new thread needs its own handler installed"), so every
//      thread entry point in this file calls
//      installThreadFatalContainment() first. (b) An exception escaping
//      every frame: VERIFIED on this CRT — it is delivered to the process
//      UEF as the MSVC C++ exception code 0xE06D7363, NOT routed through
//      std::terminate — so channel (4) is what actually closes it (the
//      UEF dies with 0xE06D7363).
//   2. abort()/SIGABRT itself (assert-style fatals, and the unreachable
//      std::abort() tails after TerminateProcess): with _WRITE_ABORT_MSG
//      and _CALL_REPORTFAULT cleared, abort() emits no message and invokes
//      no WER — but its tail is still _exit(3) -> ExitProcess -> DLL
//      detach, which raises the blocking "WHouse stays in memory" dialog
//      on a possibly-open warehouse session (the §5 teardown hazard; an
//      invisible deadlock under the SW_HIDE serve broker). The SIGABRT
//      handler (onFatalSigabrt) -> TerminateProcess(21) dies before that
//      tail — skipping ALL DLL detach.
//   3. CRT invalid-parameter calls (the *_s functions rejecting bad args)
//      and pure virtual calls: both default into _invoke_watson -> WER
//      (WerFault.exe) UNCONDITIONALLY — the abort-behavior flags do not
//      gate them. Closed by _set_invalid_parameter_handler ->
//      TerminateProcess(22) and _set_purecall_handler ->
//      TerminateProcess(23). Every path here is fatal by CRT contract —
//      the stock handlers never resume the caller — so only the death
//      style changes; nothing continuable is suppressed.
//   4. Unhandled SEH exceptions outside the VEH list — including the
//      0xE06D7363 C++ escape from ANY frame that has no handler (main or
//      a raw CreateThread worker — verified delivery: exception escapes
//      go straight to the process UEF on this CRT) — closed by
//      uefCrashHandler via SetUnhandledExceptionFilter. A record that
//      reaches the process UEF is unhandled BY DEFINITION (the process is
//      already dying); the filter TerminateProcess(ExceptionCode)s every
//      record it sees and does NOT chain into orlib20's filter (TSCrash.dll
//      spawns TSCrashReport.exe there and blocks on it — KB §14 —
//      chaining would re-open the exact channel this closes).
//
// UEF ORDERING vs orlib20 — the only part of this hardening that depends
// on foreign code:
//   - ALL orlib20 initialization happens inside `new TsDevLib` on the
//     64 MB-stack worker: LoadLibraryW(orlib20.dll) in the OrlibResolver
//     ctor (tsdev_lib.cpp), the seven support-DLL pre-loads, the patch
//     application, and OrlibInit(NULL, 1). Any UEF orlib installs — at
//     DLL attach or inside OrlibInit (KB §14 proves TSCrash.dll installs
//     one; its exact install site is not documented) — lands INSIDE that
//     ctor, on that thread.
//   - installProcessUef() therefore runs once in main() (covers the
//     pre-orlib window) and again in every worker IMMEDIATELY after `new
//     TsDevLib` returns — ours is outermost for the whole warehouse
//     session that follows. SetUnhandledExceptionFilter is last-writer-
//     wins on a single process-global slot, so each call is a full
//     re-assert, idempotent.
//   - Residual (documented, bounded): an orlib code path that RE-installs
//     its UEF AFTER construction (lazy attach / first dispatch — not
//     documented, and unprovable absent without re-running binary
//     analysis) would supersede ours until the next re-assert. The
//     1-second re-arm sentry (startUefRearmSentry — same spawn-once /
//     never-joined lifecycle as the dispatch watchdog) bounds that
//     window to one poll interval; the VEH list and the CRT guards hold
//     regardless.
//
// Exit codes 20-24 (0-11 CLI ops, 12 client markers, 13 diag watchdog,
// 14 VAS abort, 15 dispatch watchdog, 16-17 cache, 18-19 recycle — this
// block takes the next free codes; also documented in the header comment):
constexpr DWORD kExitFatalTerminate        = 20;  // std::terminate
constexpr DWORD kExitFatalAbort            = 21;  // abort()/SIGABRT
constexpr DWORD kExitFatalInvalidParameter = 22;  // CRT invalid parameter
constexpr DWORD kExitFatalPureCall         = 23;  // pure virtual call
constexpr DWORD kExitFatalNoException      = 24;  // defensive: UEF invoked
                                                  // with no exception
                                                  // record (never per the
                                                  // API contract)

// Shared non-returning tail for every fatal-containment handler.
// TerminateProcess is the ONLY exit that skips ALL DLL detach — ExitProcess
// on a possibly-open warehouse session raises the blocking "WHouse stays in
// memory" dialog (§5). The _exit tail is unreachable (TerminateProcess does
// not return) and exists only so no handler falls through into a return
// that the CRT could interpret as "continue"; it deliberately is NOT
// std::abort() — that would re-enter onFatalSigabrt.
[[noreturn]] void terminateFatal(DWORD code) {
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    _exit(static_cast<int>(code));  // unreachable — TerminateProcess does not return
}

// std::terminate handler — channel (1).
[[noreturn]] void onFatalTerminate() {
    terminateFatal(kExitFatalTerminate);
}

// SIGABRT handler — channel (2). signal()-installed; abort() reaches it
// once its message/WER steps are suppressed by _set_abort_behavior, and
// never returns past it (the _exit(3) tail is skipped entirely).
void __cdecl onFatalSigabrt(int) {
    terminateFatal(kExitFatalAbort);
}

// CRT invalid-parameter handler — channel (3a). The five report strings
// (expression/function/file/line/reserved) are nullptr/0 in release builds
// and are unused — the fixed exit code is the whole diagnostic.
void __cdecl onFatalInvalidParameter(const wchar_t*, const wchar_t*,
                                     const wchar_t*, unsigned int,
                                     uintptr_t) {
    terminateFatal(kExitFatalInvalidParameter);
}

// Pure virtual call handler — channel (3b).
void __cdecl onFatalPurecall() {
    terminateFatal(kExitFatalPureCall);
}

// Process unhandled-exception filter — channel (4). Installed outermost
// after orlib init and re-asserted by the sentry (ordering note above).
// Every record reaching it is unhandled by definition — the process is
// already dying — so it dies with the SEH code, silently: no TSCrashReport
// (orlib's filter is never chained), no WerFault, no DLL detach.
LONG WINAPI uefCrashHandler(PEXCEPTION_POINTERS ep) {
    DWORD code = kExitFatalNoException;
    if (ep && ep->ExceptionRecord) {
        code = ep->ExceptionRecord->ExceptionCode;
    }
    terminateFatal(code);
}

// Per-THREAD CRT guards — MSVC keeps the terminate handler per-thread
// (MSDN: maintained separately for each thread; each new thread needs its
// own installed), so every thread entry point calls this first. Threads in
// OTHER translation units (dispatch watchdog, cache-compress worker)
// cannot reach this anonymous-namespace helper — an escaped C++ exception
// there still dies via the process-wide SIGABRT/UEF guards below.
void installThreadFatalContainment() {
    std::set_terminate(onFatalTerminate);
}

// Per-PROCESS CRT guards — install once in main(). This state lives in OUR
// module's CRT instance (orlib20 links a different, older CRT and cannot
// overwrite it), so one install is durable. Idempotent; also installs the
// calling thread's terminate guard so main() needs no second call.
void installFatalContainment() {
    // abort() can neither print its message nor invoke WER; SIGABRT then
    // reaches onFatalSigabrt -> TerminateProcess(21), skipping _exit(3).
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::signal(SIGABRT, onFatalSigabrt);
    _set_invalid_parameter_handler(onFatalInvalidParameter);
    _set_purecall_handler(onFatalPurecall);
    installThreadFatalContainment();
}

// Install (or re-assert) our UEF outermost — last SetUnhandledExceptionFilter
// wins on the single process-global slot. See the ordering note above.
void installProcessUef() {
    SetUnhandledExceptionFilter(uefCrashHandler);
}

// One-second UEF re-arm (the ordering residual). Also re-asserts
// SEM_NOGPFAULTERRORBOX — the same process-global exposure (any loaded DLL
// could clear it). Cannot throw: pure Win32 on a POD body.
DWORD WINAPI uefRearmSentryProc(LPVOID) {
    installThreadFatalContainment();
    for (;;) {
        installProcessUef();
        SetErrorMode(SetErrorMode(0) | SEM_NOGPFAULTERRORBOX);
        Sleep(1000);
    }
}

// Spawn-once, never-joined — the thread dies with the process (identical
// lifecycle to the dispatch watchdog thread). Returns false when already
// running or CreateThread failed; the post-ctor re-asserts remain in force
// either way.
bool startUefRearmSentry() {
    static std::atomic<bool> started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) return false;
    HANDLE h = CreateThread(nullptr, 0, uefRearmSentryProc, nullptr, 0,
                            nullptr);
    if (!h) {
        started.store(false);
        return false;
    }
    CloseHandle(h);
    return true;
}

} // namespace

#ifndef TSDEV_CLI_TEST
int main(int argc, char** argv) {
    // §7.1 diagnostic build: file sink + watchdog (no-op in the production
    // build). First statement so the log covers everything that follows.
    TSDEV_DIAG_INIT();

#ifdef TSDEV_DIAG
    // §7.1 diag (run-identifiability record): every run must be identifiable from its log
    // alone — the 2026-09-02 incident run could only be identified as
    // `serve` by parsing the preserved minidump's PEB. Log the argv values
    // as-is (this is a local diagnostic tool run by the user themselves),
    // except that each argument is truncated to 200 chars and the whole list
    // is capped, so a pathological argument cannot flood the one-line
    // record. Compiled out of the production build.
    {
        std::string argvLogged;
        for (int i = 0; i < argc; i++) {
            std::string a = argv[i] ? argv[i] : "(null)";
            if (a.size() > 200) a = a.substr(0, 200) + "...";
            if (!argvLogged.empty() &&
                argvLogged.size() + 1 + a.size() > 800) {
                argvLogged += " ...(truncated)";
                break;
            }
            if (!argvLogged.empty()) argvLogged += ' ';
            argvLogged += a;
        }
        TSDEV_DIAG_LOG("run.start", "argc=%d argv=%s", argc,
                       argvLogged.c_str());
    }
#endif

    // Install VEH crash handler — must be before any orlib20 calls.
    // Catches fatal exceptions and terminates cleanly instead of spawning
    // TSCrashReport.exe (which runs elevated and can't be killed by non-elevated clients).
    AddVectoredExceptionHandler(1, vehCrashHandler);
    // Also suppress WerFault.exe as belt-and-suspenders (won't suppress TSCrashReport
    // but prevents the standard Windows crash dialog too).
    SetErrorMode(SetErrorMode(0) | SEM_NOGPFAULTERRORBOX);
    // P4 gap-e: the remaining fatal channels of OUR process — C++ terminate
    // (per-thread in the MSVC CRT), abort/SIGABRT, CRT invalid-parameter +
    // purecall WER paths, and the process UEF. orlib20's own UEF
    // (TSCrash.dll -> TSCrashReport.exe) is superseded here and re-asserted
    // outermost after every `new TsDevLib` + once a second by the sentry.
    // See the containment block comment above vehCrashHandler.
    installFatalContainment();
    installProcessUef();
    startUefRearmSentry();

    // --- Global flag pre-scan: --ts-path, --json, --pbar and -v/--verbose ---
    // Scan argv[1..argc-1] for the optional global flags and remove them
    // (and, for --ts-path, their value) from the argv array. The resulting
    // reduced argv contains only the subcommand and its positional args, so
    // the existing exact-argc subcommand dispatch below works unchanged.
    //
    // --ts-path: space-separated ("--ts-path <path>") or equals
    //            ("--ts-path=<path>") form. If given multiple times, the
    //            LAST occurrence wins.
    // --json:    boolean switch. Presence (bare "--json" or "--json=...")
    //            enables JSON output. Takes no value.
    // --pbar:    boolean switch exactly like --json. Presence (bare
    //            "--pbar" or "--pbar=...") selects the pinned stderr
    //            progress-bar rendering; repeats stay true.
    // -v/--verbose: position-free, repeatable boolean switch. Each occurrence
    //            raises the runtime log level one step (0 -> Warning,
    //            1 -> Info, >=2 -> Debug, clamped). Accepted forms: "-v",
    //            "-vv", "-vvv" (bundled shorts: any arg matching -v+, the
    //            count is the number of v chars), bare "--verbose", and
    //            "--verbose=<value>" (any value after '=' is ignored and the
    //            occurrence counts as ONE — mirrors the --json=... behavior).
    //            "Space + value" (--verbose 2) is NOT supported: the value
    //            token is not a flag and flows to reducedArgv like any other
    //            positional. Repeated separate occurrences accumulate.
    const char* tsPathOverride = nullptr;
    std::string tsPathStr;  // holds the value for the equals form
    bool jsonMode = false;
    bool pbar = false;        // --pbar (stderr progress bar)
    bool batchControl = false;  // --batch-progress-control (batch-* only)
    int batchControlCount = 0;  // occurrences; >1 rejected post-elevation
    // R6: bounded batch-control decision wait in seconds (0 disables —
    // documented escape hatch). Default 900 s. Only meaningful with
    // --batch-progress-control; a lone flag is a usage error.
    unsigned batchControlTimeoutSecs = 900;
    bool batchControlTimeoutFlagSet = false;  // --batch-control-timeout-secs seen
    int verboseCount = 0;   // accumulated -v/--verbose occurrences
    // TVS-1b: WHServer VAS-headroom guard flags (honored by batch-* and serve;
    // parsing is global like the other position-free switches).
    VasGuardConfig vasGuard;
    bool vasFreeFlagSet = false;     // --whserver-free-mb seen
    bool vasLargestFlagSet = false;  // --whserver-largest-free-mb seen
    bool traverseStrategyDependents = false;  // --traverse-strategy-dependents
    // WHServer gated auto-recycle (design tmp/RECYCLE_ENGINE_DESIGN.md §6.1):
    // presence boolean parsed in the global pre-scan like --json; the
    // TSDEV_RECYCLE_WHSERVER env mirror is OR-ed in below (env "0" never
    // vetoes an explicit flag — no negative flag form exists).
    RecycleConfig recycle;
    // TVS-3: dispatch watchdog bound in seconds (0 disables). Default 300 —
    // far above the ~3.4 s legit max single-dispatch duration, far below
    // forever. --dispatch-timeout-secs / TSDEV_DISPATCH_TIMEOUT_SECS.
    unsigned dispatchTimeoutSecs = 300;
    bool dispatchTimeoutFlagSet = false;        // --dispatch-timeout-secs seen
    // Cache phase (--backup-cache / --restore-cache): presence-true booleans
    // consumed here like --pbar; the phase runs after all validation, before
    // subcommand dispatch (fixed order backup -> restore -> subcommand).
    bool backupCacheFlag = false;
    bool restoreCacheFlag = false;
    bool noCacheCompressFlag = false;  // --no-cache-compress
    CacheBackupPolicy cachePolicy = CacheBackupPolicy::Prompt;  // default
    bool cachePolicyFlagSet = false;   // --cache-backup-policy seen

    std::vector<char*> reducedArgv;
    reducedArgv.push_back(argv[0]);  // program name

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--ts-path") {
            // Space-separated form: value is the next arg.
            if (i + 1 >= argc) {
                CliResult r;
                r.exitCode = 1;
                r.errorMessage = "--ts-path requires a directory path";
                r.humanStderr = "Error: --ts-path requires a directory path\n";
                emitOutput(r, jsonMode);
                return 1;
            }
            if (argv[i + 1][0] == '\0') {
                CliResult r;
                r.exitCode = 1;
                r.errorMessage = "--ts-path requires a non-empty directory path";
                r.humanStderr = "Error: --ts-path requires a non-empty directory path\n";
                emitOutput(r, jsonMode);
                return 1;
            }
            tsPathOverride = argv[i + 1];
            i++;  // skip the value
        } else if (arg.rfind("--ts-path=", 0) == 0) {
            // Equals form: value is everything after the '='.
            tsPathStr = arg.substr(std::strlen("--ts-path="));
            if (tsPathStr.empty()) {
                CliResult r;
                r.exitCode = 1;
                r.errorMessage = "--ts-path requires a non-empty directory path";
                r.humanStderr = "Error: --ts-path requires a non-empty directory path\n";
                emitOutput(r, jsonMode);
                return 1;
            }
            tsPathOverride = tsPathStr.c_str();
        } else if (arg == "--json" || arg.rfind("--json=", 0) == 0) {
            // Boolean switch: presence enables JSON output. Any value
            // after '=' is ignored (presence == true).
            jsonMode = true;
        } else if (arg == "--pbar" || arg.rfind("--pbar=", 0) == 0) {
            // Boolean switch like --json: presence enables the stderr
            // progress bar; any value after '=' is ignored. Repeats are
            // harmless (idempotent — the flag simply stays true).
            pbar = true;
        } else if (arg == "--batch-progress-control" ||
                   arg.rfind("--batch-progress-control=", 0) == 0) {
            // Batch-only boolean switch (presence counts; a '=' value is
            // ignored, mirroring --json). Position-free. Occurrences are
            // counted here, not rejected: a repeat is a usage error but it
            // must be reported AFTER the elevation guard below so every
            // non-elevated direct invocation still returns 11 first.
            ++batchControlCount;
            batchControl = true;
        } else if (arg == "--verbose" || arg.rfind("--verbose=", 0) == 0) {
            // Boolean switch like --json: presence counts as ONE verbosity
            // occurrence. Any value after '=' is ignored.
            verboseCount++;
        } else if (arg.size() >= 2 && arg[0] == '-' && arg[1] == 'v' &&
                   arg.find_first_not_of('v', 1) == std::string::npos) {
            // Bundled short form: -v, -vv, -vvv, ... The occurrence count is
            // the number of v chars. (Anything after the v run, e.g.
            // "-verbose" or "-vx", does not match and flows to reducedArgv.)
            verboseCount += static_cast<int>(arg.size() - 1);
        } else if (arg == "--whserver-free-mb" ||
                   arg.rfind("--whserver-free-mb=", 0) == 0) {
            // Space ("--whserver-free-mb <N>") or equals form; strict
            // decimal MB (see parseMbThreshold). Invalid -> usage error.
            std::string v;
            if (arg == "--whserver-free-mb") {
                if (i + 1 >= argc) {
                    CliResult r;
                    r.exitCode = 1;
                    r.errorMessage = "--whserver-free-mb requires a value in MB";
                    r.humanStderr = "Error: --whserver-free-mb requires a value in MB\n";
                    emitOutput(r, jsonMode);
                    return 1;
                }
                v = argv[++i];
            } else {
                v = arg.substr(std::strlen("--whserver-free-mb="));
            }
            if (!parseMbThreshold(v, vasGuard.freeThresholdMB)) {
                CliResult r;
                r.exitCode = 1;
                r.errorMessage = "invalid --whserver-free-mb value \"" + v + "\"";
                r.humanStderr = "Error: invalid --whserver-free-mb value \"" + v + "\"\n";
                emitOutput(r, jsonMode);
                return 1;
            }
            vasFreeFlagSet = true;
        } else if (arg == "--whserver-largest-free-mb" ||
                   arg.rfind("--whserver-largest-free-mb=", 0) == 0) {
            // Same value contract as --whserver-free-mb.
            std::string v;
            if (arg == "--whserver-largest-free-mb") {
                if (i + 1 >= argc) {
                    CliResult r;
                    r.exitCode = 1;
                    r.errorMessage = "--whserver-largest-free-mb requires a value in MB";
                    r.humanStderr = "Error: --whserver-largest-free-mb requires a value in MB\n";
                    emitOutput(r, jsonMode);
                    return 1;
                }
                v = argv[++i];
            } else {
                v = arg.substr(std::strlen("--whserver-largest-free-mb="));
            }
            if (!parseMbThreshold(v, vasGuard.largestThresholdMB)) {
                CliResult r;
                r.exitCode = 1;
                r.errorMessage = "invalid --whserver-largest-free-mb value \"" + v + "\"";
                r.humanStderr = "Error: invalid --whserver-largest-free-mb value \"" + v + "\"\n";
                emitOutput(r, jsonMode);
                return 1;
            }
            vasLargestFlagSet = true;
        } else if (arg == "--no-whserver-vas-guard" ||
                   arg.rfind("--no-whserver-vas-guard=", 0) == 0) {
            // Boolean switch like --json: presence disables the guard; a
            // '=' value is ignored; repeats are harmless (idempotent).
            vasGuard.enabled = false;
        } else if (arg == "--recycle-whserver" ||
                   arg.rfind("--recycle-whserver=", 0) == 0) {
            // Boolean switch like --json: presence authorizes the gated
            // WHServer recycle; a '=' value is ignored; repeats are
            // harmless (idempotent). Valid only with batch-*/serve —
            // validated after the elevation guard below.
            recycle.authorized = true;
        } else if (arg == "--traverse-strategy-dependents" ||
                   arg.rfind("--traverse-strategy-dependents=", 0) == 0) {
            // Boolean switch like --json: presence restores the legacy
            // per-item whole-library reverse-dependent traversal for
            // strategy (cat-0) roots (skipped by default). A '=' value is
            // ignored; repeats are harmless (idempotent).
            traverseStrategyDependents = true;
        } else if (arg == "--dispatch-timeout-secs" ||
                   arg.rfind("--dispatch-timeout-secs=", 0) == 0) {
            // Space ("--dispatch-timeout-secs <N>") or equals form; strict
            // decimal seconds 0..86400 (0 disables the watchdog). Invalid ->
            // usage error (see parseTimeoutSecs).
            std::string v;
            if (arg == "--dispatch-timeout-secs") {
                if (i + 1 >= argc) {
                    CliResult r;
                    r.exitCode = 1;
                    r.errorMessage = "--dispatch-timeout-secs requires a value in seconds";
                    r.humanStderr = "Error: --dispatch-timeout-secs requires a value in seconds\n";
                    emitOutput(r, jsonMode);
                    return 1;
                }
                v = argv[++i];
            } else {
                v = arg.substr(std::strlen("--dispatch-timeout-secs="));
            }
            if (!parseTimeoutSecs(v, dispatchTimeoutSecs)) {
                CliResult r;
                r.exitCode = 1;
                r.errorMessage = "invalid --dispatch-timeout-secs value \"" + v + "\"";
                r.humanStderr = "Error: invalid --dispatch-timeout-secs value \"" + v + "\"\n";
                emitOutput(r, jsonMode);
                return 1;
            }
            dispatchTimeoutFlagSet = true;
        } else if (arg == "--batch-control-timeout-secs" ||
                   arg.rfind("--batch-control-timeout-secs=", 0) == 0) {
            // Space ("--batch-control-timeout-secs <N>") or equals form;
            // strict decimal seconds 0..86400 (0 disables the bound — the
            // documented unbounded-wait escape hatch). Invalid -> usage
            // error (see parseTimeoutSecs). Requires
            // --batch-progress-control (validated post-elevation).
            std::string v;
            if (arg == "--batch-control-timeout-secs") {
                if (i + 1 >= argc) {
                    CliResult r;
                    r.exitCode = 1;
                    r.errorMessage = "--batch-control-timeout-secs requires a value in seconds";
                    r.humanStderr = "Error: --batch-control-timeout-secs requires a value in seconds\n";
                    emitOutput(r, jsonMode);
                    return 1;
                }
                v = argv[++i];
            } else {
                v = arg.substr(std::strlen("--batch-control-timeout-secs="));
            }
            if (!parseTimeoutSecs(v, batchControlTimeoutSecs)) {
                CliResult r;
                r.exitCode = 1;
                r.errorMessage = "invalid --batch-control-timeout-secs value \"" + v + "\"";
                r.humanStderr = "Error: invalid --batch-control-timeout-secs value \"" + v + "\"\n";
                emitOutput(r, jsonMode);
                return 1;
            }
            batchControlTimeoutFlagSet = true;
        } else if (arg == "--backup-cache" ||
                   arg.rfind("--backup-cache=", 0) == 0) {
            // Boolean switch like --json: presence requests the backup step;
            // a '=' value is ignored; repeats are harmless.
            backupCacheFlag = true;
        } else if (arg == "--restore-cache" ||
                   arg.rfind("--restore-cache=", 0) == 0) {
            // Boolean switch like --json: presence requests the restore step.
            restoreCacheFlag = true;
        } else if (arg == "--no-cache-compress" ||
                   arg.rfind("--no-cache-compress=", 0) == 0) {
            // Boolean switch like --json: presence suppresses the background
            // NTFS compression kick after a successful backup phase; a '='
            // value is ignored; repeats are harmless.
            noCacheCompressFlag = true;
        } else if (arg == "--cache-backup-policy" ||
                   arg.rfind("--cache-backup-policy=", 0) == 0) {
            // Space ("--cache-backup-policy <mode>") or equals form; the
            // value is one of prompt|overwrite|skip|abort (case-insensitive,
            // see parseCacheBackupPolicy). Invalid -> usage error.
            std::string v;
            if (arg == "--cache-backup-policy") {
                if (i + 1 >= argc) {
                    CliResult r;
                    r.exitCode = 1;
                    r.errorMessage = "--cache-backup-policy requires a value "
                                     "(prompt|overwrite|skip|abort)";
                    r.humanStderr = "Error: --cache-backup-policy requires a "
                                    "value (prompt|overwrite|skip|abort)\n";
                    emitOutput(r, jsonMode);
                    return 1;
                }
                v = argv[++i];
            } else {
                v = arg.substr(std::strlen("--cache-backup-policy="));
            }
            if (!parseCacheBackupPolicy(v, cachePolicy)) {
                CliResult r;
                r.exitCode = 1;
                r.errorMessage = "invalid --cache-backup-policy value \"" + v + "\"";
                r.humanStderr = "Error: invalid --cache-backup-policy value \"" + v + "\"\n";
                emitOutput(r, jsonMode);
                return 1;
            }
            cachePolicyFlagSet = true;
        } else {
            reducedArgv.push_back(argv[i]);
        }
    }

    // TVS-1b: TSDEV_WHSERVER_* env mirrors for the VAS-headroom guard,
    // applied BEFORE dispatch; explicit CLI flags win (same convention as
    // TSDEV_DIAG_WATCHDOG_SECS — a garbage/missing value keeps the
    // default). TSDEV_NO_WHSERVER_VAS_GUARD: any value other than "0"
    // disables the guard (an explicit --no-whserver-vas-guard flag also
    // wins over an env "0", matching the flag's presence semantics).
    {
        char envBuf[64] = {};
        DWORD n = GetEnvironmentVariableA("TSDEV_WHSERVER_FREE_MB", envBuf,
                                          sizeof(envBuf));
        if (!vasFreeFlagSet && n > 0 && n < sizeof(envBuf)) {
            unsigned v = 0;
            if (parseMbThreshold(envBuf, v)) vasGuard.freeThresholdMB = v;
        }
        envBuf[0] = '\0';
        n = GetEnvironmentVariableA("TSDEV_WHSERVER_LARGEST_FREE_MB", envBuf,
                                    sizeof(envBuf));
        if (!vasLargestFlagSet && n > 0 && n < sizeof(envBuf)) {
            unsigned v = 0;
            if (parseMbThreshold(envBuf, v)) vasGuard.largestThresholdMB = v;
        }
        envBuf[0] = '\0';
        n = GetEnvironmentVariableA("TSDEV_NO_WHSERVER_VAS_GUARD", envBuf,
                                    sizeof(envBuf));
        if (vasGuard.enabled && n > 0 && n < sizeof(envBuf) &&
            !(n == 1 && envBuf[0] == '0')) {
            vasGuard.enabled = false;
        }
        // TSDEV_TRAVERSE_STRATEGY_DEPENDENTS: any value other than "0"
        // restores the legacy strategy-root reverse-dependent traversal
        // (same presence semantics as TSDEV_NO_WHSERVER_VAS_GUARD; the
        // explicit flag also wins over an env "0").
        envBuf[0] = '\0';
        n = GetEnvironmentVariableA("TSDEV_TRAVERSE_STRATEGY_DEPENDENTS",
                                    envBuf, sizeof(envBuf));
        if (!traverseStrategyDependents && n > 0 && n < sizeof(envBuf) &&
            !(n == 1 && envBuf[0] == '0')) {
            traverseStrategyDependents = true;
        }
        // TSDEV_RECYCLE_WHSERVER: any value other than "0" authorizes the
        // gated WHServer recycle (same presence semantics; OR-ed — env "0"
        // does NOT veto an explicit --recycle-whserver flag, pinned in
        // design §6.1/§4.3). authorizedViaEnv records flag-vs-env source
        // so refusals attribute the right arm (env-only authorization
        // names TSDEV_RECYCLE_WHSERVER, not the flag).
        {
            const bool recycleFlagSeen = recycle.authorized;
            recycle.authorized = resolveRecycleAuthorized(recycleFlagSeen);
            recycle.authorizedViaEnv = recycle.authorized && !recycleFlagSeen;
        }
        // TVS-3: TSDEV_DISPATCH_TIMEOUT_SECS env mirror for the dispatch
        // watchdog bound; the explicit --dispatch-timeout-secs flag wins
        // (same convention as TSDEV_WHSERVER_*; a garbage/missing value
        // keeps the 300 s default). "0" disables the watchdog entirely.
        envBuf[0] = '\0';
        n = GetEnvironmentVariableA("TSDEV_DISPATCH_TIMEOUT_SECS", envBuf,
                                    sizeof(envBuf));
        if (!dispatchTimeoutFlagSet && n > 0 && n < sizeof(envBuf)) {
            unsigned v = 0;
            if (parseTimeoutSecs(envBuf, v)) {
                dispatchTimeoutSecs = v;
            }
        }
    }

    // TVS-3: start the production dispatch watchdog BEFORE any subcommand
    // dispatch. One call covers single ops, batch workers, and the serve
    // broker (spawn-once; the thread lives for the process lifetime). Every
    // orlib20 warehouse call in tsdev_lib.cpp raises the in-flight marker
    // the watchdog watches; an in-flight dispatch older than the bound — or
    // a WHServer crash/disappearance mid-flight — aborts the process with
    // exit 15. A non-elevated run exits 11 below before any dispatch can be
    // in-flight, so an early start cannot false-fire.
    tsdev::startDispatchWatchdog(dispatchTimeoutSecs);

    // TVS (CLI log levels): set the process-global library log level ONCE,
    // right after the pre-scan and BEFORE subcommand dispatch, so all later
    // diagnostics are gated consistently. Batch worker threads and the serve
    // broker are spawned after this point and inherit the level.
    tsdev_log_set_level(LogLevelFromVerbosityCount(verboseCount));

    // Same set-once process-global contract as the log level: apply the
    // strategy-root traversal switch before any TsDevLib construction.
    tsdev_set_traverse_strategy_dependents(traverseStrategyDependents);

    int reducedArgc = static_cast<int>(reducedArgv.size());
    char** reducedArgvPtr = reducedArgv.data();

    // The policy only applies to --backup-cache; an explicit
    // --cache-backup-policy without it (also under --restore-cache alone)
    // would otherwise be silently ignored — a usage error instead. Checked
    // before the no-subcommand usage error below so a bare
    // `tsdev_cli --cache-backup-policy=<mode>` reports the real problem.
    if (cachePolicyFlagSet && !backupCacheFlag) {
        CliResult r;
        r.exitCode = 1;
        r.errorMessage = "--cache-backup-policy requires --backup-cache";
        r.humanStderr = "Error: --cache-backup-policy requires --backup-cache\n";
        emitOutput(r, jsonMode);
        return 1;
    }

    // No-subcommand is a usage error UNLESS a standalone cache op
    // (--backup-cache / --restore-cache) was requested — those run without
    // a subcommand.
    if (reducedArgc < 2 && !backupCacheFlag && !restoreCacheFlag) {
        CliResult r;
        r.exitCode = 1;
        r.errorMessage = "no subcommand given";
        r.humanStderr = "";  // printUsage writes directly below
        if (jsonMode) {
            emitOutput(r, jsonMode);
        } else {
            printUsage();
        }
        return 1;
    }

    // Empty for a standalone --backup-cache/--restore-cache invocation.
    std::string subcommand =
        reducedArgc >= 2 ? reducedArgvPtr[1] : "";

    // Convert the narrow tsPathOverride to wide for the TsDevLib constructor.
    // The existing toWide() helper lives in an anonymous namespace in
    // tsdev_lib.cpp (file-local, not accessible here), so the conversion is
    // performed inline using MultiByteToWideChar (the same Windows API that
    // toWide() wraps) — no duplicate named helper is introduced.
    std::wstring tsPathWide;
    const wchar_t* tsPathWidePtr = nullptr;
    if (tsPathOverride) {
        int wlen = MultiByteToWideChar(CP_ACP, 0, tsPathOverride,
            static_cast<int>(std::strlen(tsPathOverride)),
            nullptr, 0);
        tsPathWide.resize(wlen);
        MultiByteToWideChar(CP_ACP, 0, tsPathOverride,
            static_cast<int>(std::strlen(tsPathOverride)),
            &tsPathWide[0], wlen);
        tsPathWidePtr = tsPathWide.c_str();
    }

    // Elevation guard (TVS-K.1): all subcommands require administrator
    // privileges. Placed AFTER the global flag pre-scan (so --json/--ts-path
    // are honored and the error can be emitted in JSON form) but BEFORE
    // subcommand dispatch. Excluded under TSDEV_CLI_TEST (unit tests include
    // this .cpp and define their own main()).
    if (!isElevated()) {
        if (jsonMode) {
            nlohmann::ordered_json j;
            j["success"] = false;
            j["exit_code"] = 11;
            j["error"]["code"] = "not_elevated";
            j["error"]["message"] = "this tool requires administrator privileges. "
                                    "Run from an elevated terminal (or let the Python wrapper auto-elevate).";
            std::string s = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            std::fwrite(s.c_str(), 1, s.size(), stdout);
            std::fputc('\n', stdout);
        } else {
            std::fprintf(stderr, "tsdev_cli: ERROR — this tool requires administrator privileges. "
                                 "Run from an elevated terminal (or let the Python wrapper auto-elevate).\n");
        }
        return 11;
    }

    // --pbar validations live here, AFTER the unchanged elevation guard (a
    // non-elevated caller still gets exit 11 first). The --json conflict is
    // checked first (it is mode-defining), then the serve restriction.
    // --pbar + --batch-progress-control is permitted — no interaction.
    if (pbar && jsonMode) {
        CliResult r;
        r.exitCode = 1;
        r.errorMessage = "--pbar cannot be combined with --json";
        r.humanStderr = "Error: --pbar cannot be combined with --json\n";
        emitOutput(r, jsonMode);
        return 1;
    }
    if (pbar && iequals(subcommand, "serve")) {
        CliResult r;
        r.exitCode = 1;
        r.errorMessage = "--pbar is not valid with serve";
        r.humanStderr = "tsdev_cli: --pbar is not valid with serve\n";
        emitOutput(r, jsonMode);  // jsonMode is false here; kept uniform
        return 1;
    }
    // The cache phase runs in the direct CLI process before dispatch; serve
    // is a long-lived broker, so the flags are rejected there (a future
    // request-level mechanism is out of scope).
    if ((backupCacheFlag || restoreCacheFlag) &&
        iequals(subcommand, "serve")) {
        CliResult r;
        r.exitCode = 1;
        r.errorMessage = "--backup-cache/--restore-cache are not valid with serve";
        r.humanStderr = "tsdev_cli: --backup-cache/--restore-cache are not "
                        "valid with serve\n";
        emitOutput(r, jsonMode);
        return 1;
    }

    // --batch-progress-control is batch-only and non-repeatable: the flag
    // is recognized, counted, and removed during the global scan, but both
    // validations live here, AFTER the unchanged elevation guard (a
    // non-elevated caller still gets exit 11 first).
    if (batchControlCount > 1) {
        CliResult r;
        r.exitCode = 1;
        r.errorMessage = "--batch-progress-control must not be repeated";
        r.humanStderr = "Error: --batch-progress-control must not be repeated\n";
        emitOutput(r, jsonMode);
        return 1;
    }
    // --batch-control-timeout-secs only tunes the --batch-progress-control
    // decision wait — a lone flag is a user error, not a silent no-op
    // (same convention as the --recycle-whserver combination refusals).
    if (batchControlTimeoutFlagSet && !batchControl) {
        CliResult r;
        r.exitCode = 1;
        r.errorMessage = "--batch-control-timeout-secs requires "
                         "--batch-progress-control";
        r.humanStderr = "Error: --batch-control-timeout-secs requires "
                        "--batch-progress-control\n";
        emitOutput(r, jsonMode);
        return 1;
    }
    if (batchControl) {
        // Shared batch-* subcommand list (isBatchSubcommand) — same set
        // the --recycle-whserver restriction builds on.
        if (!isBatchSubcommand(subcommand)) {
            CliResult r;
            r.exitCode = 1;
            r.errorMessage = "--batch-progress-control is only valid with "
                             "batch-* subcommands";
            r.humanStderr = "tsdev_cli: --batch-progress-control is only "
                            "valid with batch-* subcommands\n";
            if (jsonMode) {
                emitOutput(r, jsonMode);
            } else {
                std::fprintf(stderr, "%s", r.humanStderr.c_str());
                printUsage();
            }
            return 1;
        }
    }

    // --recycle-whserver (design §6.1): validated here, AFTER the unchanged
    // elevation guard (a non-elevated caller still gets exit 11 first) —
    // same convention as --batch-progress-control. Refuses up front the
    // --no-whserver-vas-guard and --batch-progress-control combinations and
    // any non-batch/non-serve subcommand.
    {
        CliResult r;
        if (!validateRecycleWhserver(recycle.authorized,
                                     recycle.authorizedViaEnv,
                                     vasGuard.enabled,
                                     batchControl, subcommand, r)) {
            if (jsonMode) {
                emitOutput(r, jsonMode);
            } else {
                std::fprintf(stderr, "%s", r.humanStderr.c_str());
            }
            return 1;
        }
    }

    // §6.3: emit the residual-race warning once per process lifetime when
    // the flag is in effect (in serve mode once per broker lifetime — a
    // respawned broker is a new process that re-emits it). Emitted after
    // validation so a refused invocation does not warn about a recycle
    // that cannot happen, and before any subcommand dispatch so it
    // precedes every process that can recycle.
    if (recycle.authorized) {
        emitRecycleWhserverWarning();
    }

    // Cache phase (--backup-cache / --restore-cache): runs after ALL
    // validation above and before any subcommand dispatch — before any
    // worker thread or TsDevLib exists — in the fixed order backup ->
    // restore -> subcommand. A failure exits before the subcommand runs;
    // a standalone invocation (no subcommand) returns its exit code here.
    if (backupCacheFlag || restoreCacheFlag) {
        const int cacheRc = runCachePhase(backupCacheFlag, restoreCacheFlag,
                                          cachePolicy, tsPathWidePtr,
                                          subcommand, jsonMode,
                                          noCacheCompressFlag);
        if (cacheRc >= 0) return cacheRc;
    }

    // --- TVS-K.4: serve subcommand ---
    // serve runs a long-lived named-pipe broker: it creates the pipe, launches
    // runServe on a 64MB-stack worker thread, and waits. It takes an optional
    // --serve-pipe-file <path> (parsed inside cmdServe). Placed before the
    // other subcommands so it is dispatched first.
    if (iequals(subcommand, "serve")) {
        return cmdServe(reducedArgc, reducedArgvPtr, jsonMode, tsPathWidePtr,
                        vasGuard, recycle);
    }

    if (iequals(subcommand, "insert")) {
        if (reducedArgc != 5) {
            CliResult r;
            r.op = "insert";
            r.exitCode = 1;
            r.errorMessage = "insert requires exactly 3 arguments: <type> <name> <source_file>";
            if (jsonMode) {
                emitOutput(r, jsonMode);
            } else {
                printUsage();
            }
            return 1;
        }
        return cmdInsert(reducedArgvPtr[2], reducedArgvPtr[3],
                         reducedArgvPtr[4], tsPathWidePtr, jsonMode, pbar);
    }

    if (iequals(subcommand, "modify")) {
        if (reducedArgc != 5) {
            CliResult r;
            r.op = "modify";
            r.exitCode = 1;
            r.errorMessage = "modify requires exactly 3 arguments: <type> <name> <source_file>";
            if (jsonMode) {
                emitOutput(r, jsonMode);
            } else {
                printUsage();
            }
            return 1;
        }
        return cmdModify(reducedArgvPtr[2], reducedArgvPtr[3],
                         reducedArgvPtr[4], tsPathWidePtr, jsonMode, pbar);
    }

    if (iequals(subcommand, "delete")) {
        if (reducedArgc != 4) {
            CliResult r;
            r.op = "delete";
            r.exitCode = 1;
            r.errorMessage = "delete requires exactly 2 arguments: <type> <name>";
            if (jsonMode) {
                emitOutput(r, jsonMode);
            } else {
                printUsage();
            }
            return 1;
        }
        return cmdDelete(reducedArgvPtr[2], reducedArgvPtr[3],
                         tsPathWidePtr, jsonMode, pbar);
    }

    if (iequals(subcommand, "exists")) {
        // exists takes <type> <name> plus an optional --case-insensitive flag.
        // --case-insensitive is exists-specific (not a global flag), so it
        // remains in reducedArgv and is parsed here. Valid forms:
        //   exists <type> <name>                        (reducedArgc == 4)
        //   exists <type> <name> --case-insensitive     (reducedArgc == 5)
        bool caseInsensitive = false;
        std::vector<char*> exArgs;
        for (int i = 2; i < reducedArgc; i++) {
            std::string a = reducedArgvPtr[i];
            if (a == "--case-insensitive") {
                caseInsensitive = true;
            } else {
                exArgs.push_back(reducedArgvPtr[i]);
            }
        }
        int exArgc = static_cast<int>(exArgs.size());
        if (exArgc != 2) {
            CliResult r;
            r.op = "exists";
            r.exitCode = 1;
            r.errorMessage = "exists requires exactly 2 arguments: <type> <name>";
            if (jsonMode) {
                emitOutput(r, jsonMode);
            } else {
                printUsage();
            }
            return 1;
        }
        return cmdExists(exArgs[0], exArgs[1], caseInsensitive,
                         tsPathWidePtr, jsonMode, pbar);
    }

    if (iequals(subcommand, "dependents")) {
        // dependents takes <type> <name>. It is a read-only query (M.4b.1).
        // Valid form: dependents <type> <name>  (reducedArgc == 4)
        if (reducedArgc != 4) {
            CliResult r;
            r.op = "dependents";
            r.exitCode = 1;
            r.errorMessage = "dependents requires exactly 2 arguments: <type> <name>";
            if (jsonMode) {
                emitOutput(r, jsonMode);
            } else {
                printUsage();
            }
            return 1;
        }
        return cmdDependents(reducedArgvPtr[2], reducedArgvPtr[3],
                             tsPathWidePtr, jsonMode, pbar);
    }

    if (iequals(subcommand, "upsert")) {
        if (reducedArgc != 5) {
            CliResult r;
            r.op = "upsert";
            r.exitCode = 1;
            r.errorMessage = "upsert requires exactly 3 arguments: <type> <name> <source_file>";
            if (jsonMode) {
                emitOutput(r, jsonMode);
            } else {
                printUsage();
            }
            return 1;
        }
        return cmdUpsert(reducedArgvPtr[2], reducedArgvPtr[3],
                         reducedArgvPtr[4], tsPathWidePtr, jsonMode, pbar);
    }

    if (iequals(subcommand, "get-source")) {
        // get-source takes <type> <name> plus an optional --output <file>.
        // --output is get-source-specific (not a global flag), so it remains
        // in reducedArgv and is parsed here. Valid forms:
        //   get-source <type> <name>                      (reducedArgc == 4)
        //   get-source <type> <name> --output <file>      (reducedArgc == 6)
        std::string outputPath;

        // Scan for --output and extract it (and its value), counting the
        // remaining positional args.
        std::vector<char*> gsArgs;
        for (int i = 2; i < reducedArgc; i++) {
            std::string a = reducedArgvPtr[i];
            if (a == "--output") {
                if (i + 1 >= reducedArgc) {
                    CliResult r;
                    r.op = "get-source";
                    r.exitCode = 1;
                    r.errorMessage = "--output requires a file path";
                    r.humanStderr = "tsdev_cli: --output requires a file path\n";
                    if (jsonMode) {
                        emitOutput(r, jsonMode);
                    } else {
                        printUsage();
                    }
                    return 1;
                }
                outputPath = reducedArgvPtr[i + 1];
                i++;  // skip the value
            } else if (a.rfind("--output=", 0) == 0) {
                outputPath = a.substr(std::strlen("--output="));
                if (outputPath.empty()) {
                    CliResult r;
                    r.op = "get-source";
                    r.exitCode = 1;
                    r.errorMessage = "--output requires a non-empty file path";
                    r.humanStderr = "tsdev_cli: --output requires a non-empty file path\n";
                    if (jsonMode) {
                        emitOutput(r, jsonMode);
                    } else {
                        printUsage();
                    }
                    return 1;
                }
            } else {
                gsArgs.push_back(reducedArgvPtr[i]);
            }
        }

        if (gsArgs.size() != 2) {
            CliResult r;
            r.op = "get-source";
            r.exitCode = 1;
            r.errorMessage = "get-source requires exactly 2 arguments: <type> <name>";
            if (jsonMode) {
                emitOutput(r, jsonMode);
            } else {
                printUsage();
            }
            return 1;
        }
        return cmdGetSource(gsArgs[0], gsArgs[1], outputPath,
                            tsPathWidePtr, jsonMode, pbar);
    }

    // --- TVS-D.3: batch subcommands ---
    // batch-insert/batch-modify/batch-delete take a single <manifest_file>
    // argument OR --batch-stdin (read the manifest from stdin instead). They
    // ALWAYS emit a JSON array on stdout (no --json needed); --json instead
    // selects the stderr progress rendering (NDJSON events instead of the
    // human BATCH lines). The global pre-scan already parsed the flag.
    if (iequals(subcommand, "batch-insert") ||
        iequals(subcommand, "batch-modify") ||
        iequals(subcommand, "batch-delete") ||
        iequals(subcommand, "batch-upsert")) {
        if (reducedArgc != 3) {
            std::string opName = subcommand;
            std::fprintf(stderr,
                "tsdev_cli: %s requires exactly 1 argument: <manifest_file> or --batch-stdin\n",
                opName.c_str());
            return 1;
        }
        std::string op;
        if (iequals(subcommand, "batch-insert"))      op = "insert";
        else if (iequals(subcommand, "batch-modify")) op = "modify";
        else if (iequals(subcommand, "batch-upsert")) op = "upsert";
        else                                          op = "delete";
        std::string arg2 = reducedArgvPtr[2];
        if (arg2 == "--batch-stdin") {
            return cmdBatch(op, std::string(), true, tsPathWidePtr, jsonMode,
                            batchControl, batchControlTimeoutSecs, pbar,
                            vasGuard, recycle);
        }
        return cmdBatch(op, arg2, false, tsPathWidePtr, jsonMode,
                        batchControl, batchControlTimeoutSecs, pbar,
                        vasGuard, recycle);
    }

    // --- TVS-I.2: directory batch subcommands ---
    // batch-insert-dir/batch-modify-dir/batch-delete-dir/batch-upsert-dir take
    // <type> <dir_path> plus an optional repeatable -t <ext> flag. They ALWAYS
    // emit a JSON array on stdout (no --json needed); --json instead selects
    // the stderr progress rendering (the global pre-scan already parsed it).
    // The -t flag is
    // directory-batch-specific: it survives the global pre-scan and is parsed
    // here (similar to --case-insensitive in the exists dispatch).
    if (iequals(subcommand, "batch-insert-dir") ||
        iequals(subcommand, "batch-modify-dir") ||
        iequals(subcommand, "batch-delete-dir") ||
        iequals(subcommand, "batch-upsert-dir")) {
        std::string op;
        if (iequals(subcommand, "batch-insert-dir"))      op = "insert";
        else if (iequals(subcommand, "batch-modify-dir")) op = "modify";
        else if (iequals(subcommand, "batch-upsert-dir")) op = "upsert";
        else                                               op = "delete";

        // Scan reducedArgv for -t <ext> pairs. Extract the extension argument,
        // add to the extensions vector, and remove both tokens from the arg
        // list. The remaining args should be: program + subcommand + type + dir
        // (argc == 4).
        std::vector<std::string> extensions;
        std::vector<char*> dirArgs;
        dirArgs.push_back(reducedArgvPtr[0]);  // program name
        dirArgs.push_back(reducedArgvPtr[1]);  // subcommand
        for (int i = 2; i < reducedArgc; i++) {
            std::string a = reducedArgvPtr[i];
            if (a == "-t") {
                // Space-separated form: the next token is the extension.
                if (i + 1 >= reducedArgc) {
                    std::fprintf(stderr,
                        "tsdev_cli: -t requires an extension argument\n");
                    return 1;
                }
                extensions.push_back(reducedArgvPtr[i + 1]);
                i++;  // skip the extension argument
            } else {
                dirArgs.push_back(reducedArgvPtr[i]);
            }
        }
        int dirArgc = static_cast<int>(dirArgs.size());
        if (dirArgc != 4) {
            std::string opName = subcommand;
            std::fprintf(stderr,
                "tsdev_cli: %s requires exactly 2 arguments: <type> <dir_path> "
                "[-t <ext>...]\n",
                opName.c_str());
            return 1;
        }
        std::string typeStr = dirArgs[2];
        std::string dirPath = dirArgs[3];

        // Resolve the type int for parseDirectory.
        ComponentType ctype;
        if (!parseType(typeStr, ctype)) {
            std::fprintf(stderr, "tsdev_cli: unknown type \"%s\" "
                         "(expected strategy/function/indicator/tradingapp/showme)\n",
                         typeStr.c_str());
            return 2;
        }
        int typeInt = static_cast<int>(ctype);

        return cmdBatchDir(op, typeInt, typeStr, dirPath, extensions,
                           tsPathWidePtr, jsonMode, batchControl,
                           batchControlTimeoutSecs, pbar,
                           vasGuard, recycle);
    }

    {
        CliResult r;
        r.exitCode = 1;
        r.errorMessage = std::string("unknown subcommand \"") + subcommand + "\"";
        r.humanStderr = std::string("tsdev_cli: unknown subcommand \"")
                        + subcommand + "\"\n";
        if (jsonMode) {
            emitOutput(r, jsonMode);
        } else {
            std::fprintf(stderr, "tsdev_cli: unknown subcommand \"%s\"\n",
                         subcommand.c_str());
            printUsage();
        }
        return 1;
    }
}
#endif  // TSDEV_CLI_TEST
