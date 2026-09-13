// test_log_levels.cpp
//
// Unit test for the runtime log-level system (TVS: CLI log levels + repeatable
// -v/--verbose).
//
// This is a PURE UNIT TEST — it does NOT load orlib20.dll, does NOT touch the
// TradeStation installation, and does NOT need a 64 MB stack for orlib20. It
// exercises:
//   - tsdev_log_get_level() / tsdev_log_set_level() / tsdev_log_message()
//     (exported by src/tsdev_lib.h, implemented in src/tsdev_lib.cpp): the
//     process-global runtime log gate. Default level = Warning.
//   - LogLevelFromVerbosityCount() (file-local helper in src/tsdev_cli.cpp):
//     the -v/--verbose occurrence-count -> level mapping incl. clamping at
//     Debug.
//   - Runtime gating of tsdev_log_message(): a Debug-level message is
//     suppressed at the default (Warning) level and present after
//     tsdev_log_set_level(Debug); an Info-level message is suppressed at
//     Warning and shown at Info; an Error-level message is always shown.
//   - tsdev_log_set_sink(TsDevLogSink) (exported by src/tsdev_lib.h,
//     implemented in src/tsdev_lib.cpp): an installed sink receives the
//     fully formatted message INSTEAD of stderr (gated-out messages stay
//     suppressed); tsdev_log_set_sink(nullptr) restores stderr output.
//
// It runs on a worker thread with a 64 MB stack only to match the project's
// test harness convention (all tests run on a large-stack worker thread); the
// stack size is harmless for a unit test.
//
// Linkage mechanism:
//   LogLevelFromVerbosityCount() lives inside an anonymous namespace in
//   src/tsdev_cli.cpp (internal linkage), so it is NOT accessible from a
//   separate translation unit. The only way to reach it is to #include the
//   .cpp directly so the test is part of the SAME translation unit. This
//   pulls in tsdev_cli.cpp's int main(); the test #defines TSDEV_CLI_TEST
//   before the include so main() is excluded by the TSDEV_CLI_TEST guard in
//   src/tsdev_cli.cpp.
//
// stderr capture mechanism:
//   tsdev_log_message() writes to the CRT stderr stream. To capture it, the
//   test duplicates the stderr file descriptor (_dup), redirects fd 2 to a
//   temp file (_dup2), runs the emit, flushes, restores the original fd, and
//   reads the temp file back. This is the standard CRT-level redirection
//   idiom on Win32/MSVC and captures everything written via
//   fprintf(stderr)/vfprintf(stderr) inside the process.
//
// Checks:
//   1. Default level is Warning (via tsdev_log_get_level()) BEFORE any set
//      call.
//   2. LogLevelFromVerbosityCount(): 0 -> Warning, 1 -> Info, 2 -> Debug,
//      3 -> Debug, 10 -> Debug (clamped at Debug).
//   3. Runtime gating at the DEFAULT level (Warning): an Error-level message
//      is present on stderr; Info- and Debug-level messages are suppressed
//      (captured stderr is exactly the Error marker line).
//   4. Runtime gating at Info: Error and Info messages present, Debug
//      suppressed (captured stderr is exactly Error + Info markers).
//   5. Runtime gating at Debug: all three levels present (captured stderr is
//      exactly Error + Info + Debug markers).
//   6. Level restored to Warning afterwards (getter check).
//   7. tsdev_log_set_sink(): with a sink installed, a Warning-level marker
//      reaches ONLY the sink (captured stderr is empty) and a gated-out
//      Debug marker is not delivered at all; after
//      tsdev_log_set_sink(nullptr) the marker reaches stderr again.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

// Exclude tsdev_cli.cpp's main() so this test can define its own.
// (main() is wrapped in the #ifndef TSDEV_CLI_TEST guard in tsdev_cli.cpp.)
#define TSDEV_CLI_TEST

// Include the CLI source directly so anonymous-namespace internals
// (LogLevelFromVerbosityCount, etc.) are visible.
#include "../src/tsdev_cli.cpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

namespace {

// Unique marker lines emitted through the log function. Unique enough that
// they cannot collide with any other output of the included TU.
const char kErrMarker[]  = "TSLL-ERR-MARKER\n";
const char kInfoMarker[] = "TSLL-INFO-MARKER\n";
const char kDbgMarker[]  = "TSLL-DBG-MARKER\n";
const char kWarnMarker[] = "TSLL-WARN-MARKER\n";

// Capture state for the tsdev_log_set_sink() check (check 7): TsDevLogSink
// is a plain function pointer (no userdata), so the captured text lives in
// these globals.
std::string g_sinkCapture;
int         g_sinkCalls = 0;
void testLogSink(const char* formattedUtf8) {
    ++g_sinkCalls;
    if (formattedUtf8) g_sinkCapture += formattedUtf8;
}

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// Emit one marker line through the exported runtime-gated log function.
void emitMarkers() {
    tsdev_log_message(TsDevLogLevel::Error, kErrMarker);
    tsdev_log_message(TsDevLogLevel::Info, kInfoMarker);
    tsdev_log_message(TsDevLogLevel::Debug, kDbgMarker);
}

// captureStderr: run `fn` with the CRT stderr file descriptor redirected to a
// temp file; return everything written to stderr during the call via `out`.
// Uses the _dup/_dup2 CRT idiom (Win32/MSVC): duplicate the current stderr
// fd, point fd 2 at the temp file, run, flush, restore, read back. Returns
// false on any CRT failure (test treats that as a fatal check failure).
bool captureStderr(void (*fn)(void*), void* ctx, std::string& out) {
    out.clear();

    char tmpPath[MAX_PATH] = {0};
    char tmpFile[MAX_PATH] = {0};
    if (GetTempPathA(MAX_PATH, tmpPath) == 0) return false;
    if (GetTempFileNameA(tmpPath, "tsll", 0, tmpFile) == 0) return false;

    FILE* tmpF = nullptr;
    if (fopen_s(&tmpF, tmpFile, "wb") != 0 || tmpF == nullptr) return false;

    std::fflush(stderr);
    int savedFd = _dup(_fileno(stderr));
    if (savedFd == -1) {
        std::fclose(tmpF);
        DeleteFileA(tmpFile);
        return false;
    }
    if (_dup2(_fileno(tmpF), _fileno(stderr)) != 0) {
        _close(savedFd);
        std::fclose(tmpF);
        DeleteFileA(tmpFile);
        return false;
    }
    // stderr's fd now owns its own duplicated handle; close our FILE* copy.
    std::fclose(tmpF);

    fn(ctx);
    std::fflush(stderr);

    // Restore the original stderr fd and read the captured bytes back.
    _dup2(savedFd, _fileno(stderr));
    _close(savedFd);

    FILE* rf = nullptr;
    if (fopen_s(&rf, tmpFile, "rb") != 0 || rf == nullptr) {
        DeleteFileA(tmpFile);
        return false;
    }
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), rf)) > 0) {
        out.append(buf, n);
    }
    std::fclose(rf);
    DeleteFileA(tmpFile);
    return true;
}

struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    std::fprintf(stdout,
                 "=== test_log_levels: runtime log gate (unit) ===\n");

    // ------------------------------------------------------------------
    // Check 1: Default level is Warning BEFORE any set call.
    // ------------------------------------------------------------------
    {
        TsDevLogLevel lvl = tsdev_log_get_level();
        std::fprintf(stdout, "  [1] default level = %d\n",
                     static_cast<int>(lvl));
        if (lvl != TsDevLogLevel::Warning) {
            ec = fail("check1: default level != Warning", 10);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: default level is Warning\n");
    }

    // ------------------------------------------------------------------
    // Check 2: LogLevelFromVerbosityCount mapping incl. clamping.
    //   0 -> Warning, 1 -> Info, 2 -> Debug, 3 -> Debug, 10 -> Debug.
    // ------------------------------------------------------------------
    {
        struct Case { int count; TsDevLogLevel expected; };
        const Case cases[] = {
            { 0, TsDevLogLevel::Warning },
            { 1, TsDevLogLevel::Info    },
            { 2, TsDevLogLevel::Debug   },
            { 3, TsDevLogLevel::Debug   },
            {10, TsDevLogLevel::Debug   },
        };
        for (const Case& c : cases) {
            TsDevLogLevel got = LogLevelFromVerbosityCount(c.count);
            std::fprintf(stdout, "  [2] verbosity %d -> level %d\n",
                         c.count, static_cast<int>(got));
            if (got != c.expected) {
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "check2: verbosity %d mapped to level %d "
                              "(expected %d)", c.count,
                              static_cast<int>(got),
                              static_cast<int>(c.expected));
                ec = fail(msg, 20 + c.count);
                return 0;
            }
        }
        std::fprintf(stdout,
                     "    PASS: verbosity-count mapping correct (clamped)\n");
    }

    // ------------------------------------------------------------------
    // Check 3: Runtime gating at the DEFAULT level (Warning).
    //   Error marker present; Info and Debug markers suppressed. The
    //   captured stderr must be EXACTLY the Error marker line.
    // ------------------------------------------------------------------
    {
        std::string captured;
        if (!captureStderr([](void*) { emitMarkers(); }, nullptr, captured)) {
            ec = fail("check3: stderr capture setup failed", 30);
            return 0;
        }
        std::fprintf(stdout, "  [3] captured at Warning: \"%s\"",
                     captured.c_str());
        if (captured.find(kErrMarker) == std::string::npos) {
            ec = fail("check3: Error-level message not shown at default "
                      "(Warning) level", 31);
            return 0;
        }
        if (captured.find(kInfoMarker) != std::string::npos) {
            ec = fail("check3: Info-level message shown at default "
                      "(Warning) level", 32);
            return 0;
        }
        if (captured.find(kDbgMarker) != std::string::npos) {
            ec = fail("check3: Debug-level message shown at default "
                      "(Warning) level", 33);
            return 0;
        }
        if (captured != std::string(kErrMarker)) {
            ec = fail("check3: captured stderr is not exactly the Error "
                      "marker line", 34);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: Warning level shows errors only\n");
    }

    // ------------------------------------------------------------------
    // Check 4: Runtime gating at Info.
    //   Error and Info markers present; Debug suppressed. Captured stderr
    //   must be exactly Error + Info markers.
    // ------------------------------------------------------------------
    {
        tsdev_log_set_level(TsDevLogLevel::Info);
        std::string captured;
        if (!captureStderr([](void*) { emitMarkers(); }, nullptr, captured)) {
            tsdev_log_set_level(TsDevLogLevel::Warning);
            ec = fail("check4: stderr capture setup failed", 40);
            return 0;
        }
        std::fprintf(stdout, "  [4] captured at Info: \"%s\"",
                     captured.c_str());
        if (captured.find(kErrMarker) == std::string::npos) {
            tsdev_log_set_level(TsDevLogLevel::Warning);
            ec = fail("check4: Error-level message not shown at Info level",
                      41);
            return 0;
        }
        if (captured.find(kInfoMarker) == std::string::npos) {
            tsdev_log_set_level(TsDevLogLevel::Warning);
            ec = fail("check4: Info-level message suppressed at Info level",
                      42);
            return 0;
        }
        if (captured.find(kDbgMarker) != std::string::npos) {
            tsdev_log_set_level(TsDevLogLevel::Warning);
            ec = fail("check4: Debug-level message shown at Info level", 43);
            return 0;
        }
        if (captured != std::string(kErrMarker) + kInfoMarker) {
            tsdev_log_set_level(TsDevLogLevel::Warning);
            ec = fail("check4: captured stderr is not exactly Error + Info "
                      "markers", 44);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: Info level shows errors + info\n");
    }

    // ------------------------------------------------------------------
    // Check 5: Runtime gating at Debug.
    //   All three markers present. Captured stderr must be exactly
    //   Error + Info + Debug markers.
    // ------------------------------------------------------------------
    {
        tsdev_log_set_level(TsDevLogLevel::Debug);
        std::string captured;
        if (!captureStderr([](void*) { emitMarkers(); }, nullptr, captured)) {
            tsdev_log_set_level(TsDevLogLevel::Warning);
            ec = fail("check5: stderr capture setup failed", 50);
            return 0;
        }
        std::fprintf(stdout, "  [5] captured at Debug: \"%s\"",
                     captured.c_str());
        if (captured != std::string(kErrMarker) + kInfoMarker + kDbgMarker) {
            tsdev_log_set_level(TsDevLogLevel::Warning);
            ec = fail("check5: captured stderr is not exactly Error + Info "
                      "+ Debug markers", 51);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: Debug level shows all levels\n");
    }

    // ------------------------------------------------------------------
    // Check 6: Level restored to Warning (clean state after the gating
    // checks).
    // ------------------------------------------------------------------
    {
        tsdev_log_set_level(TsDevLogLevel::Warning);
        TsDevLogLevel lvl = tsdev_log_get_level();
        std::fprintf(stdout, "  [6] restored level = %d\n",
                     static_cast<int>(lvl));
        if (lvl != TsDevLogLevel::Warning) {
            ec = fail("check6: restored level != Warning", 60);
            return 0;
        }
        std::fprintf(stdout, "    PASS: level setter/getter round-trip\n");
    }

    // ------------------------------------------------------------------
    // Check 7: tsdev_log_set_sink() routes a gated message to the sink
    //   INSTEAD of stderr; tsdev_log_set_sink(nullptr) restores stderr.
    //   The level is Warning (restored in check 6): a Warning-level marker
    //   passes the gate and must reach ONLY the sink while installed; a
    //   Debug-level marker is gated out and must not reach the sink.
    // ------------------------------------------------------------------
    {
        g_sinkCapture.clear();
        g_sinkCalls = 0;
        tsdev_log_set_sink(&testLogSink);
        std::string captured;
        if (!captureStderr([](void*) {
                tsdev_log_message(TsDevLogLevel::Warning, kWarnMarker);
                tsdev_log_message(TsDevLogLevel::Debug, kDbgMarker);
            }, nullptr, captured)) {
            tsdev_log_set_sink(nullptr);
            ec = fail("check7: stderr capture setup failed", 70);
            return 0;
        }
        if (g_sinkCalls != 1 ||
            g_sinkCapture != std::string(kWarnMarker)) {
            tsdev_log_set_sink(nullptr);
            ec = fail("check7: sink did not receive exactly the formatted "
                      "Warning message (Debug must stay gated out)", 71);
            return 0;
        }
        if (!captured.empty()) {
            tsdev_log_set_sink(nullptr);
            ec = fail("check7: message reached stderr while a sink was "
                      "installed", 72);
            return 0;
        }
        // nullptr restores the default stderr routing.
        tsdev_log_set_sink(nullptr);
        g_sinkCapture.clear();
        g_sinkCalls = 0;
        if (!captureStderr([](void*) {
                tsdev_log_message(TsDevLogLevel::Warning, kWarnMarker);
            }, nullptr, captured)) {
            ec = fail("check7: stderr capture setup failed after sink "
                      "reset", 73);
            return 0;
        }
        if (captured != std::string(kWarnMarker)) {
            ec = fail("check7: message did not reach stderr after sink "
                      "reset to nullptr", 74);
            return 0;
        }
        if (g_sinkCalls != 0) {
            ec = fail("check7: sink still invoked after reset to nullptr",
                      75);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: log sink replaces stderr; nullptr "
                     "restores it\n");
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

// SEH wrapper — POD-only (no C++ objects with destructors) so __try is legal.
DWORD WINAPI threadProc(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    __try {
        runTest(param);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr,
                     "\n[FATAL] Unhandled SEH exception 0x%08X\n",
                     GetExceptionCode());
        st->exitCode = 99;
    }
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_log_levels: runtime log gate (unit) ===\n");

    TestState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  threadProc, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    int exitCode = st.exitCode;
    std::fprintf(stdout,
                 "\n=== test_log_levels complete (exit %d) ===\n", exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
