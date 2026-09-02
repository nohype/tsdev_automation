// diag_log.cpp
//
// Implementation of the §7.1 diagnostic file sink. See diag_log.h for the
// contract. This TU is compiled only into the diagnostic build (TSDEV_DIAG);
// the production build neither compiles it nor links it.

#include "diag_log.h"

#ifdef TSDEV_DIAG

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace tsdev {
namespace diag {

namespace {

// Serializes writers so records stay atomic. Recursive (the watchdog owns it
// across its multi-record dump session; CRITICAL_SECTION re-entrancy makes
// its internal record writes safe).
CRITICAL_SECTION g_sinkLock;
bool g_lockInitialized = false;

HANDLE g_file = INVALID_HANDLE_VALUE;
std::string g_logPathUtf8;  // full path of the current log file (UTF-8)

LARGE_INTEGER g_qpcStart;  // QPC at sink init (elapsed-seconds origin)
LARGE_INTEGER g_freq;

// Watchdog progress (§7.1 2f): QPC of the last site record. Watchdog-written
// records do NOT update this (they must not keep the watchdog fed).
std::atomic<long long> g_lastRecordQpc{0};

bool g_initialized = false;

// Copy payload into `dst`, replacing CR/LF so every record stays exactly one
// line (the atomicity argument and the tests rely on the one-line invariant).
void sanitizeOneLine(const char* src, char* dst, size_t dstSize) {
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && o + 1 < dstSize; i++) {
        char c = src[i];
        if (c == '\n' || c == '\r') {
            if (o + 2 >= dstSize) break;
            dst[o++] = '\\';
            dst[o++] = (c == '\n') ? 'n' : 'r';
        } else {
            dst[o++] = c;
        }
    }
    dst[o] = '\0';
}

// Core write. Caller holds g_sinkLock. Single WriteFile per record so the
// record is contiguous and flushed (FILE_FLAG_WRITE_THROUGH) when the call
// returns — it survives a crash or hang-kill of the process.
void writeRecordLocked(const char* site, const char* payload, bool progress) {
    if (!g_initialized || g_file == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double secs = static_cast<double>(now.QuadPart - g_qpcStart.QuadPart) /
                  static_cast<double>(g_freq.QuadPart);

    char clean[1024];
    sanitizeOneLine(payload, clean, sizeof(clean));

    char line[1600];
    int n = std::snprintf(line, sizeof(line),
                          "[%12.6f] qpc=%llu tid=%lu %s | %s\n", secs,
                          static_cast<unsigned long long>(now.QuadPart),
                          static_cast<unsigned long>(GetCurrentThreadId()),
                          site, clean);
    if (n <= 0) return;
    if (static_cast<size_t>(n) >= sizeof(line)) n = sizeof(line) - 1;

    // Review F7(a): a failing WriteFile silently drops this record (its
    // return value and written count are deliberately not checked). This is
    // diag-of-diag and acceptable: the sink must never crash or fail the CLI
    // because its own diagnostics failed, and a full disk / closed handle
    // losing one diag record does not change any behavior. Everything else
    // (timestamps, ordering of the surviving records) is unaffected.
    DWORD written = 0;
    WriteFile(g_file, line, static_cast<DWORD>(n), &written, nullptr);
    if (progress) {
        g_lastRecordQpc.store(now.QuadPart, std::memory_order_relaxed);
    }
}

void formatAndWriteLocked(const char* site, const char* fmt, va_list ap,
                          bool progress) {
    char payload[1024];
    std::vsnprintf(payload, sizeof(payload), fmt, ap);
    payload[sizeof(payload) - 1] = '\0';
    writeRecordLocked(site, payload, progress);
}

}  // namespace

bool initSink() {
    if (!g_lockInitialized) {
        InitializeCriticalSectionAndSpinCount(&g_sinkLock, 0x0400);
        g_lockInitialized = true;
    }

    EnterCriticalSection(&g_sinkLock);
    // Re-initialization support (tests): close the previous file first.
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
        g_initialized = false;
    }

    // Directory: TSDEV_DIAG_DIR overrides the process working directory.
    // Review F7(b): an overlong TSDEV_DIAG_DIR value (>= MAX_PATH, i.e. it
    // did not fit into the buffer above) silently falls back to the process
    // working directory — the log is still created (and its path printed to
    // stderr), just not in the requested directory. Accepted diag-of-diag
    // behavior: say-so here instead of failing the run over a log path.
    wchar_t dir[MAX_PATH];
    DWORD dirLen = GetEnvironmentVariableW(L"TSDEV_DIAG_DIR", dir, MAX_PATH);
    bool haveOverride = (dirLen > 0 && dirLen < MAX_PATH);
    if (!haveOverride) {
        dirLen = GetCurrentDirectoryW(MAX_PATH, dir);
    }
    if (dirLen == 0 || dirLen >= MAX_PATH) {
        LeaveCriticalSection(&g_sinkLock);
        return false;
    }
    if (haveOverride) {
        CreateDirectoryW(dir, nullptr);  // no-op when it already exists
    }

    wchar_t fname[MAX_PATH];
    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD pid = GetCurrentProcessId();
    int n = std::swprintf(fname, MAX_PATH,
                          L"%ls\\tsdev_diag_%lu_%04u%02u%02u_%02u%02u%02u.log",
                          dir, pid, st.wYear, st.wMonth, st.wDay, st.wHour,
                          st.wMinute, st.wSecond);
    if (n <= 0) {
        LeaveCriticalSection(&g_sinkLock);
        return false;
    }

    // FILE_FLAG_WRITE_THROUGH: every WriteFile goes to stable storage before
    // returning, so records survive process death (§7.1 2a).
    HANDLE h = CreateFileW(fname, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&g_sinkLock);
        return false;
    }
    g_file = h;

    char narrow[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, fname, -1, narrow, sizeof(narrow), nullptr,
                        nullptr);
    g_logPathUtf8 = narrow;

    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_qpcStart);
    g_lastRecordQpc.store(g_qpcStart.QuadPart, std::memory_order_relaxed);
    g_initialized = true;
    LeaveCriticalSection(&g_sinkLock);

    // Print the log file path once to stderr at startup (§7.1 2a).
    std::fprintf(stderr, "tsdev_diag: log file: %s\n", g_logPathUtf8.c_str());
    std::fflush(stderr);
    return true;
}

const char* logPath() {
    return g_logPathUtf8.c_str();
}

void logRecordf(const char* site, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    EnterCriticalSection(&g_sinkLock);
    formatAndWriteLocked(site, fmt, ap, /*progress=*/true);
    LeaveCriticalSection(&g_sinkLock);
    va_end(ap);
}

void logRecordNoProgressf(const char* site, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    EnterCriticalSection(&g_sinkLock);
    formatAndWriteLocked(site, fmt, ap, /*progress=*/false);
    LeaveCriticalSection(&g_sinkLock);
    va_end(ap);
}

void lockSink() {
    if (!g_lockInitialized) {
        InitializeCriticalSectionAndSpinCount(&g_sinkLock, 0x0400);
        g_lockInitialized = true;
    }
    EnterCriticalSection(&g_sinkLock);
}

void unlockSink() {
    LeaveCriticalSection(&g_sinkLock);
}

long long lastRecordQpc() {
    return g_lastRecordQpc.load(std::memory_order_relaxed);
}

long long qpcFrequency() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

}  // namespace diag
}  // namespace tsdev

#endif  // TSDEV_DIAG
