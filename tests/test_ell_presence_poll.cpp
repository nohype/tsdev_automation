// test_ell_presence_poll.cpp
//
// Pure unit test for the bounded post-verify .ell presence poll
// (src/ell_presence_poll.h, Slice-2 hardening #3).
//
// Background: doInsert step 5 (src/tsdev_cli.cpp) made ONE ellFileExists()
// call after verifyModule and failed loudly (exit 9) on a miss. The writer
// is MDVault::SaveModule inside verifyModule, whose OS-level write can lag
// or fail while ELMDCore still reports success (Fault A family), so a single
// immediate check can false-negative on a transient. The fix polls
// check-then-sleep with a hard bound, then fails loudly with the SAME exit-9
// contract on timeout (never masks Fault A).
//
// This test is PURE: no orlib20, no TsDevLib, no TS install. It drives the
// seam (tsdev::pollUntilPresent) with GetFileAttributesA predicates on temp
// files plus counting/no-op sleeps, and one real-Sleep timing case at the
// production budget. RequiresTs = $false.
//
// Cases:
//   1. immediate-present: file exists before the first check -> true, exactly
//      1 check, 0 sleeps, fast (zero added latency in the common case).
//   2. delayed-present: file appears on the 3rd check (lagging writer) ->
//      true, 3 checks, 2 sleeps (poll rescues the transient).
//   3. never-present (small budget): -> false, exactly maxSleeps+1 checks and
//      maxSleeps sleeps (bounded, no infinite wait).
//   4. never-present (production budget, real Sleep): -> false after
//      ~10 s (timeout preserves fail-loud; timing recorded).
//
// Exit code: 0 only if ALL cases pass; distinct nonzero codes per failure.

#include "../src/ell_presence_poll.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__);        \
            ++g_failures;                                             \
        }                                                             \
    } while (0)

std::string tempFilePath(const char* tag) {
    char dir[MAX_PATH];
    DWORD n = GetTempPathA(static_cast<DWORD>(sizeof(dir)), dir);
    if (n == 0 || n >= sizeof(dir)) {
        return std::string();
    }
    char name[MAX_PATH];
    std::snprintf(name, sizeof(name), "ellpoll_%lu_%s.tmp",
                  static_cast<unsigned long>(GetCurrentProcessId()), tag);
    return std::string(dir) + name;
}

bool fileExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

void createEmptyFile(const std::string& path) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }
}

// Case 1: file present before the first check.
int caseImmediatePresent() {
    std::string path = tempFilePath("immediate");
    if (path.empty()) {
        std::printf("FAIL: immediate: no temp path\n");
        return 1;
    }
    DeleteFileA(path.c_str());
    createEmptyFile(path);

    int sleeps = 0;
    int checks = 0;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = tsdev::pollUntilPresent(
        [&]() { return fileExists(path); },
        [&]() { ++sleeps; },
        tsdev::kEllPresencePollMaxSleeps, &checks);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    DeleteFileA(path.c_str());

    CHECK(ok, "immediate: expected true");
    CHECK(checks == 1, "immediate: expected exactly 1 check");
    CHECK(sleeps == 0, "immediate: expected 0 sleeps");
    std::printf("PASS: immediate-present ok=%d checks=%d sleeps=%d elapsed=%lldms\n",
                static_cast<int>(ok), checks, sleeps,
                static_cast<long long>(ms));
    return 0;
}

// Case 2: file appears on the 3rd check (lagging writer simulation).
int caseDelayedPresent() {
    std::string path = tempFilePath("delayed");
    if (path.empty()) {
        std::printf("FAIL: delayed: no temp path\n");
        return 2;
    }
    DeleteFileA(path.c_str());

    int existsCalls = 0;
    int sleeps = 0;
    int checks = 0;
    bool ok = tsdev::pollUntilPresent(
        [&]() {
            ++existsCalls;
            if (existsCalls == 3) {
                createEmptyFile(path);  // lagging write lands here
            }
            return fileExists(path);
        },
        [&]() { ++sleeps; },
        tsdev::kEllPresencePollMaxSleeps, &checks);

    DeleteFileA(path.c_str());

    CHECK(ok, "delayed: expected true");
    CHECK(checks == 3, "delayed: expected exactly 3 checks");
    CHECK(sleeps == 2, "delayed: expected exactly 2 sleeps");
    std::printf("PASS: delayed-present ok=%d checks=%d sleeps=%d\n",
                static_cast<int>(ok), checks, sleeps);
    return 0;
}

// Case 3: never present, small budget -> bounded false.
int caseNeverPresentBounded() {
    const int maxSleeps = 3;
    int sleeps = 0;
    int checks = 0;
    bool ok = tsdev::pollUntilPresent(
        []() { return false; },
        [&]() { ++sleeps; },
        maxSleeps, &checks);

    CHECK(!ok, "bounded: expected false");
    CHECK(checks == maxSleeps + 1, "bounded: expected maxSleeps+1 checks");
    CHECK(sleeps == maxSleeps, "bounded: expected exactly maxSleeps sleeps");
    std::printf("PASS: never-present-bounded ok=%d checks=%d sleeps=%d\n",
                static_cast<int>(ok), checks, sleeps);
    return 0;
}

// Case 4: never present, production budget, real Sleep -> false after ~10 s.
int caseProductionTimeout() {
    int checks = 0;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = tsdev::pollUntilPresent(
        []() { return false; },
        []() { Sleep(tsdev::kEllPresencePollIntervalMs); },
        tsdev::kEllPresencePollMaxSleeps, &checks);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    const long long lo = static_cast<long long>(tsdev::kEllPresencePollMaxSleeps) *
                         static_cast<long long>(tsdev::kEllPresencePollIntervalMs);
    CHECK(!ok, "timeout: expected false");
    CHECK(checks == tsdev::kEllPresencePollMaxSleeps + 1,
          "timeout: expected maxSleeps+1 checks");
    CHECK(ms >= lo - 500, "timeout: returned too early (not bounded below)");
    CHECK(ms <= lo + 5000, "timeout: took far longer than budget");
    std::printf("PASS: never-present-timeout ok=%d checks=%d elapsed=%lldms (budget~%lldms)\n",
                static_cast<int>(ok), checks,
                static_cast<long long>(ms), lo);
    return 0;
}

}  // namespace

int main() {
    // Production-budget self-check: 20 x 500 ms = ~10 s by construction.
    CHECK(tsdev::kEllPresencePollMaxSleeps == 20, "budget: maxSleeps must be 20");
    CHECK(tsdev::kEllPresencePollIntervalMs == 500, "budget: interval must be 500ms");

    if (caseImmediatePresent() != 0) return 1;
    if (caseDelayedPresent() != 0) return 2;
    if (caseNeverPresentBounded() != 0) return 3;
    if (caseProductionTimeout() != 0) return 4;
    if (g_failures != 0) {
        std::printf("FAILURES: %d\n", g_failures);
        return 5;
    }
    std::printf("ALL PASS: ell presence poll (4 cases)\n");
    return 0;
}
