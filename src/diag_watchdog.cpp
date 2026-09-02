// diag_watchdog.cpp
//
// Implementation of the §7.1 diagnostic watchdog. See diag_watchdog.h for
// the contract. This TU is compiled only into the diagnostic build
// (TSDEV_DIAG); the production build neither compiles it nor links it.
//
// Known limitation of the stack records (§7.1 2d(1), review F4): on x86,
// FPO (frame-pointer-omitted) frames without a findable PDB can truncate a
// StackWalk64 walk early (field-observed: a walk that produced one
// our-exe frame plus the thread-start anchor). The full-memory minidump
// written by the same fire path is the fallback evidence — it contains all
// thread contexts and stacks for complete offline reconstruction.

#include "diag_watchdog.h"

#ifdef TSDEV_DIAG

#include "diag_log.h"

#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace tsdev {
namespace diag {

namespace {

// Watchdog exit code: 13. The C++ CLI already uses 0-11 (tsdev_cli.cpp header
// comment: 10 = CreateThread failure, 11 = not elevated) and the Python
// client uses 12 for its own non-elevated error
// (src/tsdev_automation/exceptions.py, ElevationRequiredError), so 13 is the
// first exit code unused across the whole toolchain.
const int WATCHDOG_EXIT_CODE = 13;

// Default watchdog threshold (§7.1 2d): seconds without a site record before
// the watchdog fires. TSDEV_DIAG_WATCHDOG_SECS overrides; a value <= 0
// disables the watchdog entirely (review F3a — long-idle roles like the
// `serve` broker idle by design).
const double kDefaultThresholdSecs = 60.0;

// Heartbeat interval while an idle-by-design phase is active (review F3b).
// Capped at threshold/3 in watchdogProc so a heartbeat lands well before a
// small threshold override could fire (the smoke run's 8 s env override must
// not fire before the first heartbeat).
const double kIdleHeartbeatSecs = 15.0;

std::atomic<void*> g_warehouse{nullptr};
double g_thresholdSecs = kDefaultThresholdSecs;
bool g_watchdogDisabled = false;  // TSDEV_DIAG_WATCHDOG_SECS <= 0 (review F3a)

// Idle-by-design phase flag (review F3b): set while the process is in a
// legitimately idle wait (e.g. the serve broker blocked in ConnectNamedPipe).
// Plain flag + tag pointer; the tag is written before the release-store and
// only ever points to string literals, so the acquire-load in the watchdog
// thread always reads a valid tag.
std::atomic<bool> g_idlePhaseActive{false};
const char* g_idlePhaseTag = "(none)";

// --- SEH-protected raw read of a 4-byte warehouse-struct field -------------
// Raw form with an explanatory comment (AGENTS.md): the offsets are the
// documented warehouse-struct layout (docs/KNOWLEDGE_BASE.md §2) and the
// pointer was obtained via the resolved GetWarehouse export. Read-only
// diagnostics; SEH guards against a stale pointer.
bool sehReadWhDword(void* wh, int offset, unsigned int* out) {
    __try {
        *out = *reinterpret_cast<unsigned int*>(
            reinterpret_cast<char*>(wh) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 0-timeout probe of a protocol handle (KB §3): WAIT_OBJECT_0 = signaled /
// acquirable, WAIT_TIMEOUT = not signaled / held, WAIT_FAILED = invalid
// handle. For the mutex, a successful 0-timeout acquire is released again
// immediately (the blocked dispatcher holds it while waiting on the
// completion event, so WAIT_TIMEOUT is the expected healthy-hang state).
//
// Review F6 (bounded side effect, accepted): the probe WAITS WITH 0 TIMEOUT
// on the protocol handles, so if +0x6A/+0x6E are AUTO-RESET events and
// currently signaled, the probe CONSUMES the signal. Consequence is bounded:
// the evidence value is logged BEFORE consumption and the process is about
// to terminate anyway — but the probe can destroy an "evDone signaled but
// waiter stuck" state mid-diagnosis (it is then no longer observable in a
// later probe; the minidump retains the pre-probe handle values).
const char* probeHandleState(unsigned int handleRaw, bool isMutex) {
    HANDLE h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handleRaw));
    DWORD r = WaitForSingleObject(h, 0);
    if (isMutex && (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED)) {
        ReleaseMutex(h);  // we acquired it with the 0-timeout probe; undo
    }
    switch (r) {
        case WAIT_OBJECT_0:  return "signaled";
        case WAIT_TIMEOUT:   return isMutex ? "held" : "not-signaled";
        case WAIT_FAILED:    return "invalid-handle";
        default:             return "other";
    }
}

// --- §7.1 2d(1): all thread stacks as module+offset ------------------------

void dumpOneThreadStack(DWORD tid) {
    HANDLE hThread = OpenThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
            THREAD_QUERY_INFORMATION,
        FALSE, tid);
    if (!hThread) {
        logRecordNoProgressf("watchdog.stack",
                             "tid=%lu open-failed err=%lu", tid,
                             GetLastError());
        return;
    }
    if (SuspendThread(hThread) == static_cast<DWORD>(-1)) {
        logRecordNoProgressf("watchdog.stack",
                             "tid=%lu suspend-failed err=%lu", tid,
                             GetLastError());
        CloseHandle(hThread);
        return;
    }

    CONTEXT ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(hThread, &ctx)) {
        logRecordNoProgressf("watchdog.stack",
                             "tid=%lu context-failed err=%lu", tid,
                             GetLastError());
        ResumeThread(hThread);
        CloseHandle(hThread);
        return;
    }

    STACKFRAME64 frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.AddrPC.Offset = ctx.Eip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Ebp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Esp;
    frame.AddrStack.Mode = AddrModeFlat;

    int depth = 0;
    while (depth < 128) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_I386, GetCurrentProcess(),
                         hThread, &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64,
                         nullptr)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) break;

        DWORD64 pc = frame.AddrPC.Offset;
        DWORD64 base = SymGetModuleBase64(GetCurrentProcess(), pc);
        if (base) {
            char modName[256] = "(unknown)";
            IMAGEHLP_MODULE64 mod;
            std::memset(&mod, 0, sizeof(mod));
            mod.SizeOfStruct = sizeof(mod);
            if (SymGetModuleInfo64(GetCurrentProcess(), base, &mod) &&
                mod.ImageName[0]) {
                const char* slash = std::strrchr(mod.ImageName, '\\');
                std::strncpy(modName, slash ? slash + 1 : mod.ImageName,
                             sizeof(modName) - 1);
                modName[sizeof(modName) - 1] = '\0';
            }
            logRecordNoProgressf("watchdog.stack",
                                 "tid=%lu frame=%d pc=0x%08X %s+0x%08X", tid,
                                 depth, static_cast<unsigned>(pc), modName,
                                 static_cast<unsigned>(pc - base));
        } else {
            logRecordNoProgressf("watchdog.stack",
                                 "tid=%lu frame=%d pc=0x%08X (no module)",
                                 tid, depth, static_cast<unsigned>(pc));
        }
        depth++;
    }
    if (depth == 0) {
        logRecordNoProgressf("watchdog.stack",
                             "tid=%lu walk-empty eip=0x%08X", tid,
                             static_cast<unsigned>(ctx.Eip));
    }

    ResumeThread(hThread);
    CloseHandle(hThread);
}

void dumpAllThreadStacks() {
    // Review F4: SymInitialize(..., TRUE) ran at watchdog start, BEFORE
    // orlib20.dll was loaded, so the dbghelp module list did not contain
    // orlib20 and its frames logged as "(no module)" (field-confirmed:
    // 0x101D4205 etc. = orlib20+0x1D4205). Refresh the module list here, at
    // fire time, so the one module that matters most is attributed as
    // module+offset (§7.1 2d(1)). Failure is non-fatal: stacks then fall
    // back to raw addresses — say so in the log.
    if (!SymRefreshModuleList(GetCurrentProcess())) {
        logRecordNoProgressf("watchdog.stacks",
                             "module-list-refresh-failed err=%lu "
                             "(frames may log as (no module))",
                             GetLastError());
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,
                                           GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        logRecordNoProgressf("watchdog.stacks",
                             "snapshot-failed err=%lu", GetLastError());
        return;
    }
    DWORD self = GetCurrentThreadId();  // exclude the watchdog itself
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
            if (te.th32ThreadID == self) continue;
            dumpOneThreadStack(te.th32ThreadID);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

// --- §7.1 2d(2): warehouse protocol state ----------------------------------

void logWarehouseState() {
    void* wh = g_warehouse.load(std::memory_order_relaxed);
    if (!wh) {
        // §7.1 2d(2) fallback: the wh pointer is NOT cleanly obtainable (the
        // hang is inside OrlibInit before GetWarehouse returned, or the
        // registration call failed). Log that explicitly and rely on stacks +
        // minidump — do NOT hack orlib20 internals to recover it.
        logRecordNoProgressf(
            "watchdog.warehouse",
            "wh=unavailable (not registered — hang is pre-init or "
            "GetWarehouse failed); relying on stacks+minidump");
        return;
    }

    // Raw reads of documented warehouse-struct fields (docs/KNOWLEDGE_BASE.md
    // §2): +0x04 current cmd, +0x6A completion event, +0x6E wake event,
    // +0x72 mutex, +0x76 worker tid. Read-only; the pointer came from the
    // resolved GetWarehouse export.
    unsigned int cmd = 0, evDone = 0, evWake = 0, mutex = 0, workerTid = 0;
    bool okCmd = sehReadWhDword(wh, 0x04, &cmd);
    bool okDone = sehReadWhDword(wh, 0x6A, &evDone);
    bool okWake = sehReadWhDword(wh, 0x6E, &evWake);
    bool okMutex = sehReadWhDword(wh, 0x72, &mutex);
    bool okTid = sehReadWhDword(wh, 0x76, &workerTid);

    logRecordNoProgressf(
        "watchdog.warehouse",
        "wh=0x%08X cmd(+0x04)=%u ok=%d evDone(+0x6A)=0x%08X state=%s "
        "evWake(+0x6E)=0x%08X state=%s mutex(+0x72)=0x%08X state=%s "
        "workerTid(+0x76)=%u ok=%d",
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(wh)), cmd,
        okCmd ? 1 : 0, evDone, probeHandleState(evDone, false), evWake,
        probeHandleState(evWake, false), mutex, probeHandleState(mutex, true),
        workerTid, okTid ? 1 : 0);
}

// --- §7.1 2d(3): minidump ---------------------------------------------------

std::string dumpPathFor(const char* logPath) {
    std::string p = logPath;
    if (p.size() > 4 && p.compare(p.size() - 4, 4, ".log") == 0) {
        p.resize(p.size() - 4);
    }
    p += ".dmp";
    return p;
}

void writeMinidump(const std::string& dumpPath) {
    // Flags chosen (§7.1 2d(3) "full memory sufficient for offline stack/wh
    // analysis"): MiniDumpNormal (thread stacks + contexts) |
    // MiniDumpWithThreadInfo (per-thread times) | MiniDumpWithFullMemory
    // (all committed memory, including the shared SHARE_WO warehouse
    // section, so the warehouse/WHServer state is fully analyzable offline).
    HANDLE hFile = CreateFileA(dumpPath.c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        logRecordNoProgressf("watchdog.minidump",
                             "create-failed err=%lu path=%s", GetLastError(),
                             dumpPath.c_str());
        return;
    }
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                hFile,
                                static_cast<MINIDUMP_TYPE>(
                                    MiniDumpNormal | MiniDumpWithThreadInfo |
                                    MiniDumpWithFullMemory),
                                nullptr, nullptr, nullptr);
    logRecordNoProgressf("watchdog.minidump", "ok=%d path=%s", ok ? 1 : 0,
                         dumpPath.c_str());
    CloseHandle(hFile);
}

// --- §7.1 2e: WHServer liveness samples -------------------------------------

struct WhSampleCtx {
    const std::vector<DWORD>* pids;
    int windows;
    int hung;
};

BOOL CALLBACK whWindowEnum(HWND hwnd, LPARAM lp) {
    WhSampleCtx* ctx = reinterpret_cast<WhSampleCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    for (DWORD p : *ctx->pids) {
        if (p == pid) {
            ctx->windows++;
            if (IsHungAppWindow(hwnd)) ctx->hung++;
            break;
        }
    }
    return TRUE;
}

void whserverSample() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        logRecordNoProgressf("whserver.sample",
                             "process-snapshot-failed err=%lu",
                             GetLastError());
        return;
    }
    std::vector<DWORD> pids;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"WHServer.exe") == 0) {
                pids.push_back(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    if (pids.empty()) {
        logRecordNoProgressf("whserver.sample", "present=0");
        return;
    }

    std::string pidList;
    for (size_t i = 0; i < pids.size(); i++) {
        char b[16];
        std::snprintf(b, sizeof(b), "%lu%s", pids[i],
                      i + 1 < pids.size() ? "," : "");
        pidList += b;
    }

    // Responsiveness via top-level windows of those pids + IsHungAppWindow.
    // A windowless WHServer can only be reported as present — say so.
    WhSampleCtx ctx{&pids, 0, 0};
    EnumWindows(whWindowEnum, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.windows == 0) {
        logRecordNoProgressf(
            "whserver.sample",
            "present=1 pids=%s windows=0 (windowless; responsiveness not "
            "determinable — presence only)",
            pidList.c_str());
    } else {
        logRecordNoProgressf("whserver.sample",
                             "present=1 pids=%s windows=%d hung=%d",
                             pidList.c_str(), ctx.windows, ctx.hung);
    }
}

// --- Watchdog thread ---------------------------------------------------------

void fire(double idleSecs) {
    // Hold the sink lock across the whole dump session: no other writer can
    // be suspended while holding the mutex (it is ours), so the sequence is
    // atomic and deadlock-free. CRITICAL_SECTION re-entrancy makes the
    // internal logRecordNoProgressf calls safe.
    lockSink();

    logRecordNoProgressf("watchdog.fired",
                         "idleSecs=%.3f thresholdSecs=%.1f", idleSecs,
                         g_thresholdSecs);

    dumpAllThreadStacks();  // §7.1 2d(1)
    logWarehouseState();    // §7.1 2d(2)

    unlockSink();

    std::string dumpPath = dumpPathFor(logPath());
    writeMinidump(dumpPath);  // §7.1 2d(3)

    // §7.1 2d(4): abort loudly with a distinct exit code.
    logRecordNoProgressf("watchdog.abort", "exit=%d", WATCHDOG_EXIT_CODE);
    std::fprintf(stderr,
                 "tsdev_diag: WATCHDOG FIRED — no log progress for %.1f s "
                 "(threshold %.1f s). All thread stacks + warehouse state "
                 "were appended to the log:\n"
                 "  %s\n"
                 "minidump:\n"
                 "  %s\n"
                 "Terminating with exit code %d (tsdev_diag watchdog).\n",
                 idleSecs, g_thresholdSecs, logPath(), dumpPath.c_str(),
                 WATCHDOG_EXIT_CODE);
    std::fflush(stderr);
    // TerminateProcess, not ExitProcess (raw form with explanatory comment,
    // AGENTS.md): ExitProcess runs DLL_PROCESS_DETACH for every loaded DLL,
    // and orlib20's detach path on a process holding an open warehouse raises
    // the BLOCKING dialog "TradeStation internal error, WHouse is going to
    // stay in memory (1)" (observed in the 2026-09-02 incident,
    // tmp/exp/whouse_incident/) — the loud abort then hangs on that dialog
    // until it is dismissed. All evidence is already durable at this point
    // (every record is a single FILE_FLAG_WRITE_THROUGH WriteFile, the
    // minidump is closed, stderr is flushed), so the process must die
    // immediately WITHOUT running any DLL detach. TerminateProcess is the
    // established mechanism in this codebase (vehCrashHandler,
    // src/tsdev_cli.cpp, uses the identical call).
    TerminateProcess(GetCurrentProcess(), WATCHDOG_EXIT_CODE);
}

DWORD WINAPI watchdogProc(LPVOID) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    long long thresholdTicks =
        static_cast<long long>(g_thresholdSecs *
                               static_cast<double>(freq.QuadPart));

    // Review F3b: heartbeat interval while an idle-by-design phase is
    // active. Capped at threshold/3 so a heartbeat (which resets the
    // progress timestamp) lands well before the fire threshold could be
    // reached with a small TSDEV_DIAG_WATCHDOG_SECS override (the smoke run
    // ran with 8 s → heartbeat every ~2.7 s). With the 1 s tick below this
    // holds for thresholds >= 3 s; a sub-3-second threshold is aggressive
    // enough that firing during an idle phase is the requested behavior.
    double heartbeatSecs = kIdleHeartbeatSecs;
    if (!g_watchdogDisabled && g_thresholdSecs / 3.0 < heartbeatSecs) {
        heartbeatSecs = g_thresholdSecs / 3.0;
    }
    long long heartbeatTicks = static_cast<long long>(
        heartbeatSecs * static_cast<double>(freq.QuadPart));

    unsigned tick = 0;
    for (;;) {
        Sleep(1000);
        tick++;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        long long last = lastRecordQpc();
        long long idleTicks = now.QuadPart - last;
        if (!g_watchdogDisabled && idleTicks >= thresholdTicks) {
            fire(static_cast<double>(idleTicks) /
                 static_cast<double>(freq.QuadPart));
            return 0;  // TerminateProcess inside fire(); unreachable
        }

        // Review F3b: while the process is in an idle-by-design phase (e.g.
        // the serve broker blocked in ConnectNamedPipe waiting for pipe
        // clients), emit a periodic PROGRESS heartbeat (logRecordf — it
        // updates the watchdog progress timestamp, so a healthy idle broker
        // never reaches the fire threshold and never looks "stalled" in the
        // log). The 2026-09-02 smoke run was a healthy idle `serve` aborted
        // at 8.9 s idle; this heartbeat is the fix. Once the phase ends (a
        // client connected and dispatch work begins), the heartbeat stops
        // and normal watchdog behavior resumes.
        if (g_idlePhaseActive.load(std::memory_order_acquire) &&
            idleTicks >= heartbeatTicks) {
            logRecordf("watchdog.idle-heartbeat",
                       "phase=%s idleSecs=%.1f (idle-by-design; watchdog fed)",
                       g_idlePhaseTag,
                       static_cast<double>(idleTicks) /
                           static_cast<double>(freq.QuadPart));
        }

        // §7.1 2e: liveness sample every 5 s while idle. These records do
        // NOT update the watchdog progress timestamp (logRecordNoProgressf),
        // otherwise the watchdog could never fire.
        if (tick % 5 == 0) {
            whserverSample();
        }
    }
}

}  // namespace

void startWatchdog() {
    // Threshold: env TSDEV_DIAG_WATCHDOG_SECS overrides the 60 s default.
    // Review F3a: a parsed value <= 0 DISABLES the watchdog entirely (no
    // fire path) — long-idle roles like the `serve` broker idle by design
    // and must never be aborted for idling. A value that does not parse as
    // a number, or an unset/too-long variable, keeps the 60 s default.
    // Review N1: strtod also accepts "1e999" (→ inf) and "nan"; a
    // non-finite value would make the static_cast to threshold ticks in
    // watchdogProc UB (typically INT64_MIN on MSVC/x86 → fires on the
    // first tick), so values must be finite and within the sane bound
    // (0 < v <= 86400.0, i.e. 24 h); anything else keeps the 60 s default.
    char buf[64] = {};
    DWORD n = GetEnvironmentVariableA("TSDEV_DIAG_WATCHDOG_SECS", buf,
                                      sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        char* end = nullptr;
        double v = std::strtod(buf, &end);
        if (end != buf) {  // parses as a number
            if (v <= 0.0) {
                g_watchdogDisabled = true;
            } else if (std::isfinite(v) && v <= 86400.0) {
                g_thresholdSecs = v;
            }
            // else: non-finite (inf/NaN) or above the 86400.0 s bound —
            // garbage input; keep the 60 s default (g_thresholdSecs is
            // still kDefaultThresholdSecs here).
        }
    }

    // Module symbols for the module+offset stack records. Non-fatal on
    // failure (stacks fall back to raw addresses). The module list is
    // refreshed again at fire time (review F4 — orlib20 loads later).
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);

    HANDLE h = CreateThread(nullptr, 0, watchdogProc, nullptr, 0, nullptr);
    if (h) {
        CloseHandle(h);  // the thread runs for the process lifetime
    }
}

void setWarehousePointer(void* wh) {
    g_warehouse.store(wh, std::memory_order_relaxed);
}

void idlePhaseBegin(const char* tag) {
    g_idlePhaseTag = tag;  // string literal; written before the flag
    g_idlePhaseActive.store(true, std::memory_order_release);
}

void idlePhaseEnd() {
    g_idlePhaseActive.store(false, std::memory_order_release);
}

void diagInit() {
    if (!initSink()) {
        std::fprintf(stderr,
                     "tsdev_diag: ERROR — could not create the diagnostic "
                     "log file (TSDEV_DIAG_DIR unwritable?); continuing "
                     "WITHOUT diagnostics and WITHOUT the watchdog\n");
        std::fflush(stderr);
        return;
    }
    startWatchdog();  // parses TSDEV_DIAG_WATCHDOG_SECS, then starts the thread
    if (g_watchdogDisabled) {
        // Review F3a: say explicitly that the watchdog is off and why.
        logRecordf("diag.init",
                   "pid=%lu watchdog=disabled (TSDEV_DIAG_WATCHDOG_SECS<=0: "
                   "long-idle roles like the serve broker idle by design)",
                   GetCurrentProcessId());
    } else {
        logRecordf("diag.init", "pid=%lu watchdogSecs=%.1f",
                   GetCurrentProcessId(), g_thresholdSecs);
    }
}

}  // namespace diag
}  // namespace tsdev

#endif  // TSDEV_DIAG
