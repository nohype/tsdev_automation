// test_diag_log.cpp
//
// Pure unit test for the §7.1 diagnostic file sink (src/diag_log.h,
// src/diag_log.cpp). Covers the three required sink behaviors:
//   1. A record is present in the log file IMMEDIATELY after the log call
//      (before any close/flush at exit) — i.e. every record is flushed per
//      write and survives process death.
//   2. Atomic interleaving under concurrent writers from multiple threads:
//      every record lands as one intact line, none lost, none torn.
//   3. TSDEV_DIAG_DIR overrides the log directory; without it the log is
//      created in the process working directory.
//
// The test never constructs TsDevLib, loads orlib20.dll, or touches a
// TradeStation installation (the sink is plain Win32 file I/O).

#include "../src/diag_log.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

int fail(const char* message, int code) {
    std::fprintf(stderr, "[FAIL] %s (code=%d)\n", message, code);
    return code;
}

// Create a unique empty directory under %TEMP% and return its path.
std::string makeTempDir(const char* tag) {
    char base[MAX_PATH];
    UINT n = GetTempPathA(MAX_PATH, base);
    if (n == 0 || n >= MAX_PATH) return "";
    std::string dir = std::string(base) + "tsdev_diag_test_" + tag + "_" +
                      std::to_string(GetCurrentProcessId());
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

// Read the whole file (opens with share flags compatible with the sink's
// open write handle, so the read works while the sink keeps it open).
bool readWholeFile(const std::string& path, std::string& out) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) {
        out.append(buf, got);
    }
    CloseHandle(h);
    return true;
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_diag_log: diagnostic file sink (unit) ===\n");

    // --- Test 1: record appears in the file immediately after the log call
    // (no close/flush at exit involved). ---
    std::string dir1 = makeTempDir("flush");
    if (dir1.empty()) return fail("could not create temp dir", 10);
    SetEnvironmentVariableA("TSDEV_DIAG_DIR", dir1.c_str());
    if (!tsdev::diag::initSink()) {
        return fail("initSink should succeed with a writable TSDEV_DIAG_DIR",
                    11);
    }
    std::string path1 = tsdev::diag::logPath();
    if (path1.empty()) return fail("logPath should be non-empty after init", 12);
    if (path1.find(dir1) == std::string::npos) {
        return fail("log file should be created inside TSDEV_DIAG_DIR", 13);
    }
    // One log file per run: tsdev_diag_<pid>_<yyyymmdd_hhmmss>.log
    if (path1.find("tsdev_diag_") == std::string::npos ||
        path1.find(".log") == std::string::npos) {
        return fail("log file name should match tsdev_diag_<pid>_<ts>.log", 14);
    }

    tsdev::diag::logRecordf("test.flush", "hello %d", 42);

    std::string content1;
    if (!readWholeFile(path1, content1)) {
        return fail("log file should be readable while the sink holds it open",
                    15);
    }
    if (!contains(content1, "test.flush") || !contains(content1, "hello 42")) {
        return fail("record must be flushed to the file immediately after the "
                    "log call (per-write flush)",
                    16);
    }
    std::fprintf(stdout,
                 "  [1] PASS: record present in file immediately after the "
                 "log call (per-write flush)\n");

    // --- Test 2: atomic interleaving under concurrent writers. ---
    std::string dir2 = makeTempDir("concurrent");
    SetEnvironmentVariableA("TSDEV_DIAG_DIR", dir2.c_str());
    if (!tsdev::diag::initSink()) {
        return fail("initSink should succeed for the concurrency phase", 20);
    }
    std::string path2 = tsdev::diag::logPath();

    const int kWriters = 8;
    const int kRecordsPerWriter = 250;
    {
        std::vector<std::thread> threads;
        for (int w = 0; w < kWriters; w++) {
            threads.emplace_back([w, kRecordsPerWriter]() {
                for (int i = 0; i < kRecordsPerWriter; i++) {
                    tsdev::diag::logRecordf("test.concurrent",
                                            "writer=%d seq=%d", w, i);
                }
            });
        }
        for (auto& t : threads) t.join();
    }

    std::string content2;
    if (!readWholeFile(path2, content2)) {
        return fail("concurrency-phase log file should be readable", 21);
    }

    // Every line must be intact (no torn records), each writer must have
    // written each seq exactly once (no loss, no duplication).
    std::map<std::pair<int, int>, int> seen;
    int concurrentLines = 0;
    size_t pos = 0;
    while (pos < content2.size()) {
        size_t eol = content2.find('\n', pos);
        if (eol == std::string::npos) {
            return fail("file should end with a complete line", 22);
        }
        std::string line = content2.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.find("test.concurrent") == std::string::npos) continue;

        // The payload follows the "site | " separator.
        size_t sep = line.find(" | ");
        if (sep == std::string::npos) {
            return fail("record must contain the site|payload separator", 26);
        }
        int writer = -1;
        int seq = -1;
        if (std::sscanf(line.c_str() + sep + 3, "writer=%d seq=%d", &writer,
                        &seq) != 2 ||
            writer < 0 || writer >= kWriters || seq < 0 ||
            seq >= kRecordsPerWriter) {
            return fail("every concurrent record must be one intact line "
                        "(no torn writes)",
                        23);
        }
        seen[{writer, seq}]++;
        concurrentLines++;
    }
    if (concurrentLines != kWriters * kRecordsPerWriter) {
        std::fprintf(stderr, "  concurrent lines: %d (expected %d)\n",
                     concurrentLines, kWriters * kRecordsPerWriter);
        return fail("no concurrent record may be lost or duplicated", 24);
    }
    for (const auto& kv : seen) {
        if (kv.second != 1) {
            return fail("each (writer, seq) record must appear exactly once",
                        25);
        }
    }
    std::fprintf(stdout,
                 "  [2] PASS: %d concurrent records from %d threads, all "
                 "atomic, none lost\n",
                 concurrentLines, kWriters);

    // --- Test 3: TSDEV_DIAG_DIR override + process-working-directory
    // default. ---
    std::string dir3 = makeTempDir("override");
    SetEnvironmentVariableA("TSDEV_DIAG_DIR", dir3.c_str());
    if (!tsdev::diag::initSink()) {
        return fail("initSink should succeed for the override phase", 30);
    }
    if (std::string(tsdev::diag::logPath()).find(dir3) == std::string::npos) {
        return fail("TSDEV_DIAG_DIR must override the log directory", 31);
    }

    // Default (env var unset): the process working directory.
    std::string dir4 = makeTempDir("cwd");
    char prevCwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, prevCwd);
    SetCurrentDirectoryA(dir4.c_str());
    SetEnvironmentVariableA("TSDEV_DIAG_DIR", nullptr);
    if (!tsdev::diag::initSink()) {
        SetCurrentDirectoryA(prevCwd);
        return fail("initSink should succeed with the working-directory "
                    "default",
                    32);
    }
    std::string path4 = tsdev::diag::logPath();
    SetCurrentDirectoryA(prevCwd);
    if (path4.find(dir4) == std::string::npos) {
        return fail("without TSDEV_DIAG_DIR the log must be created in the "
                    "process working directory",
                    33);
    }
    std::fprintf(stdout,
                 "  [3] PASS: TSDEV_DIAG_DIR override + working-directory "
                 "default\n");

    // Cleanup: remove the log files and temp dirs created by this test.
    for (const std::string& dir : {dir1, dir2, dir3, dir4}) {
        WIN32_FIND_DATAA fd;
        std::string pattern = dir + "\\*";
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    DeleteFileA((dir + "\\" + fd.cFileName).c_str());
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryA(dir.c_str());
    }

    std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    return 0;
}
