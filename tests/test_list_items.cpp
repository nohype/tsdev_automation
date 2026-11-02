// test_list_items.cpp
//
// Unit test (RED-GREEN TDD) for the pure name-listing orchestration
// collectTypeNames() declared in src/whserver_names.h: the testable core of
// TsDevLib::listItems, driven against a FAKE IWhNameSource (no orlib20, no
// WHServer, no TradeStation). The real COM provider (WhComNameSource) binds
// the seam via the proven WHServer IWHouse chain and is exercised only by
// the live spot-check, not here.
//
// This is a PURE UNIT TEST - it does NOT load orlib20.dll, does NOT touch the
// TradeStation installation, and does NOT construct TsDevLib. It runs on a
// worker thread with a 64 MB stack only to match the project's test harness
// convention (all tests run on a large-stack worker thread).
//
// Mechanism under test (tmp/enum_mechanism_report.md Sec. 2):
//   count(type) = ProgramsCountByType; the walk visits ids 1 .. NextId-1
//   (sparse id space, vacant ids skipped) and collects IProgram::Name
//   verbatim per non-null id.
//
// Checks:
//   1. Verbatim names (case, spaces, punctuation) in walk order - NO
//      sorting; sparse gaps skipped; prior out-vector contents discarded.
//   2. Duplicate names are NOT deduplicated.
//   3. Empty type (count 0) -> success + zero names + no id walk.
//   4. whType mapping: all five ComponentType values reach the source as the
//      expanded whType (Strategy=0, Function=1, Indicator=2, ShowMe=1282,
//      TradingApp=5122) on every seam call.
//   5. Count/element consistency: matching counts emit no warning; a
//      mismatch in either direction (count > returned included) SUCCEEDS
//      with the WALK result (the names vector length is authoritative) and
//      reports a Warning through the log sink.
//   6. Failure path: a source error (false return) or a source exception on
//      any seam call -> returns false and outNames is left EMPTY
//      (documented state; partial names are discarded).
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

#include "whserver_names.h"

#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

using namespace tsdev;

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

void printNames(const char* label, const std::vector<std::string>& names) {
    std::fprintf(stdout, "  %s (%u):", label,
                 static_cast<unsigned>(names.size()));
    for (const auto& n : names) {
        std::fprintf(stdout, " [%s]", n.c_str());
    }
    std::fprintf(stdout, "\n");
}

// Compare collected names against an expected list (exact order, exact
// bytes). Returns false on any mismatch.
bool namesEqual(const std::vector<std::string>& got,
                const char* const* expected, size_t expectedCount) {
    if (got.size() != expectedCount) return false;
    for (size_t i = 0; i < expectedCount; i++) {
        if (got[i] != std::string(expected[i])) return false;
    }
    return true;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// --- Log capture (count-consistency reporting) -------------------------------
// collectTypeNames reports a count/element mismatch through
// tsdev_log_message(Warning, ...); the test routes the process-global log
// sink to this capture so the report itself is assertable.
std::string g_capturedLog;

void captureSink(const char* formattedUtf8) {
    if (formattedUtf8) g_capturedLog += formattedUtf8;
}

void captureBegin() {
    g_capturedLog.clear();
    tsdev_log_set_sink(&captureSink);
}

void captureEnd() {
    tsdev_log_set_sink(nullptr);
    g_capturedLog.clear();
}

// --- Fake provider -----------------------------------------------------------
// Scripted IWhNameSource: sparse id -> name rows plus per-call failure
// injection and per-call argument recording (whType assertions).
struct FakeSource : public IWhNameSource {
    std::vector<std::pair<int, std::string>> rows;
    int  countOverride = -1;   // >= 0 overrides the reported count
    int  nextIdOverride = 0;   // > 0 overrides the reported NextId
    bool failCount = false;
    bool failNextId = false;
    int  failNameAtId = 0;     // getProgramName returns false for this id
    int  throwAtNameId = 0;    // getProgramName throws for this id
    bool throwAtCount = false; // getTypeCount throws

    std::vector<int> countTypes;  // whType seen by getTypeCount
    std::vector<int> nextIdTypes; // whType seen by getNextId
    std::vector<int> nameTypes;   // whType seen by getProgramName

    bool getTypeCount(int whType, int& outCount) override {
        countTypes.push_back(whType);
        if (throwAtCount) throw std::runtime_error("fake count failure");
        if (failCount) return false;
        outCount = (countOverride >= 0)
                       ? countOverride
                       : static_cast<int>(rows.size());
        return true;
    }

    bool getNextId(int whType, int& outNextId) override {
        nextIdTypes.push_back(whType);
        if (failNextId) return false;
        int maxId = 0;
        for (const auto& r : rows) {
            if (r.first > maxId) maxId = r.first;
        }
        outNextId = (nextIdOverride > 0) ? nextIdOverride : maxId + 1;
        return true;
    }

    bool getProgramName(int whType, int id, bool& outFound,
                        std::string& outName) override {
        nameTypes.push_back(whType);
        if (throwAtNameId != 0 && id == throwAtNameId) {
            throw std::runtime_error("fake name failure");
        }
        if (failNameAtId != 0 && id == failNameAtId) return false;
        for (const auto& r : rows) {
            if (r.first == id) {
                outFound = true;
                outName = r.second;
                return true;
            }
        }
        outFound = false;
        outName.clear();
        return true;
    }
};

struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    std::fprintf(stdout,
                 "=== test_list_items: collectTypeNames (fake provider) ===\n");

    // ------------------------------------------------------------------
    // Check 1: verbatim names in walk order (NO sorting), sparse gaps
    // skipped, prior out-vector contents discarded.
    // ------------------------------------------------------------------
    {
        FakeSource src;
        src.rows.push_back(std::make_pair(2, std::string("BTA_ELITE_SmartCode_GEN3")));
        src.rows.push_back(std::make_pair(5, std::string("bounce2.0")));
        src.rows.push_back(std::make_pair(9, std::string("RCI (Spearman's RCC)")));
        src.rows.push_back(std::make_pair(11, std::string("52 Wk Channel")));
        src.rows.push_back(std::make_pair(14, std::string("%Chg")));

        std::vector<std::string> names;
        names.push_back("stale");
        const bool ok = collectTypeNames(src, ComponentType::Strategy, names);
        printNames("walk", names);
        const char* expected[] = { "BTA_ELITE_SmartCode_GEN3", "bounce2.0",
                                   "RCI (Spearman's RCC)", "52 Wk Channel",
                                   "%Chg" };
        if (!ok) {
            ec = fail("walk must succeed", 10);
            return 0;
        }
        if (!namesEqual(names, expected, 5)) {
            ec = fail("names wrong (verbatim/order/gaps/stale-clear)", 11);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: verbatim names in walk order, gaps skipped\n");
    }

    // ------------------------------------------------------------------
    // Check 2: duplicate names are NOT deduplicated.
    // ------------------------------------------------------------------
    {
        FakeSource src;
        src.rows.push_back(std::make_pair(1, std::string("Dup")));
        src.rows.push_back(std::make_pair(3, std::string("Dup")));
        src.rows.push_back(std::make_pair(4, std::string("Other")));
        src.rows.push_back(std::make_pair(7, std::string("Dup")));

        std::vector<std::string> names;
        const bool ok = collectTypeNames(src, ComponentType::Function, names);
        printNames("no-dedup", names);
        const char* expected[] = { "Dup", "Dup", "Other", "Dup" };
        if (!ok) {
            ec = fail("duplicate walk must succeed", 20);
            return 0;
        }
        if (!namesEqual(names, expected, 4)) {
            ec = fail("duplicate names were deduplicated or reordered", 21);
            return 0;
        }
        std::fprintf(stdout, "    PASS: no dedup\n");
    }

    // ------------------------------------------------------------------
    // Check 3: empty type -> success + zero names + no id walk.
    // ------------------------------------------------------------------
    {
        FakeSource src;  // count 0 (no rows)
        std::vector<std::string> names;
        names.push_back("stale");
        bool ok = collectTypeNames(src, ComponentType::TradingApp, names);
        printNames("empty", names);
        if (!ok || !names.empty()) {
            ec = fail("empty type must succeed with 0 names", 30);
            return 0;
        }
        if (!src.nextIdTypes.empty() || !src.nameTypes.empty()) {
            ec = fail("empty type must not run the id walk", 31);
            return 0;
        }

        // A reported count of 0 is authoritative for emptiness: rows present
        // but count 0 -> still zero names, walk skipped.
        FakeSource src2;
        src2.rows.push_back(std::make_pair(1, std::string("Ghost")));
        src2.countOverride = 0;
        names.push_back("stale");
        ok = collectTypeNames(src2, ComponentType::Indicator, names);
        printNames("count=0", names);
        if (!ok || !names.empty()) {
            ec = fail("count=0 must succeed with 0 names", 32);
            return 0;
        }
        if (!src2.nameTypes.empty()) {
            ec = fail("count=0 must not run the id walk", 33);
            return 0;
        }
        std::fprintf(stdout, "    PASS: empty type -> 0 names\n");
    }

    // ------------------------------------------------------------------
    // Check 4: whType mapping - all five ComponentType values reach the
    // source as the expanded whType on every seam call.
    // ------------------------------------------------------------------
    {
        struct Case { ComponentType type; int whType; };
        const Case cases[] = {
            { ComponentType::Strategy,   0 },
            { ComponentType::Function,   1 },
            { ComponentType::Indicator,  2 },
            { ComponentType::ShowMe,     1282 },
            { ComponentType::TradingApp, 5122 },
        };
        for (const auto& c : cases) {
            FakeSource src;
            src.rows.push_back(std::make_pair(1, std::string("Solo")));
            std::vector<std::string> names;
            if (!collectTypeNames(src, c.type, names)) {
                ec = fail("mapping walk must succeed", 40);
                return 0;
            }
            if (src.countTypes.size() != 1 ||
                src.countTypes[0] != c.whType ||
                src.nextIdTypes.size() != 1 ||
                src.nextIdTypes[0] != c.whType ||
                src.nameTypes.size() != 1 ||
                src.nameTypes[0] != c.whType) {
                std::fprintf(stderr,
                             "  expected whType=%d seen count=%d nextId=%d name=%d\n",
                             c.whType,
                             src.countTypes.empty() ? -1 : src.countTypes[0],
                             src.nextIdTypes.empty() ? -1 : src.nextIdTypes[0],
                             src.nameTypes.empty() ? -1 : src.nameTypes[0]);
                ec = fail("whType mapping wrong", 41);
                return 0;
            }
        }
        std::fprintf(stdout,
                     "    PASS: whType mapping 0/1/2/1282/5122 on every call\n");
    }

    // ------------------------------------------------------------------
    // Check 5: count/element consistency. Matching counts -> no warning.
    // Mismatch (either direction) -> success with the walk result (names
    // length authoritative) + a Warning report.
    // ------------------------------------------------------------------
    {
        FakeSource src;
        src.rows.push_back(std::make_pair(1, std::string("A")));
        src.rows.push_back(std::make_pair(2, std::string("B")));
        std::vector<std::string> names;
        captureBegin();
        bool ok = collectTypeNames(src, ComponentType::Strategy, names);
        const std::string logEqual = g_capturedLog;
        captureEnd();
        if (!ok || names.size() != 2 || !logEqual.empty()) {
            ec = fail("consistent counts must succeed silently", 50);
            return 0;
        }

        // count > returned (the drift direction named in the task).
        FakeSource drift;
        drift.rows.push_back(std::make_pair(1, std::string("A")));
        drift.rows.push_back(std::make_pair(2, std::string("B")));
        drift.rows.push_back(std::make_pair(4, std::string("C")));
        drift.countOverride = 5;
        names.clear();
        captureBegin();
        ok = collectTypeNames(drift, ComponentType::Function, names);
        const std::string logDrift = g_capturedLog;
        captureEnd();
        printNames("count>returned", names);
        const char* expectedDrift[] = { "A", "B", "C" };
        if (!ok) {
            ec = fail("count>returned must NOT fail (names are authoritative)",
                      51);
            return 0;
        }
        if (!namesEqual(names, expectedDrift, 3)) {
            ec = fail("count>returned must return the walked names", 52);
            return 0;
        }
        if (!contains(logDrift, "5") || !contains(logDrift, "3")) {
            std::fprintf(stderr, "  captured: %s\n", logDrift.c_str());
            ec = fail("count>returned must log a warning with both counts",
                      53);
            return 0;
        }

        // returned > count: same graceful contract.
        FakeSource rev;
        rev.rows.push_back(std::make_pair(1, std::string("X")));
        rev.rows.push_back(std::make_pair(2, std::string("Y")));
        rev.rows.push_back(std::make_pair(3, std::string("Z")));
        rev.countOverride = 1;
        names.clear();
        captureBegin();
        ok = collectTypeNames(rev, ComponentType::Indicator, names);
        const std::string logRev = g_capturedLog;
        captureEnd();
        printNames("returned>count", names);
        if (!ok || names.size() != 3 || logRev.empty()) {
            ec = fail("returned>count must succeed with a warning", 54);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: names length authoritative, drift warned\n");
    }

    // ------------------------------------------------------------------
    // Check 6: failure path - source error or source exception on ANY seam
    // call -> false + outNames EMPTY (documented state, partials discarded).
    // ------------------------------------------------------------------
    {
        FakeSource src;
        src.rows.push_back(std::make_pair(1, std::string("A")));
        src.rows.push_back(std::make_pair(2, std::string("B")));
        src.failNameAtId = 2;  // fail mid-walk, after "A" was collected
        std::vector<std::string> names;
        names.push_back("stale");
        bool ok = collectTypeNames(src, ComponentType::Strategy, names);
        printNames("failName", names);
        if (ok || !names.empty()) {
            ec = fail("getProgramName error -> false + empty outNames", 60);
            return 0;
        }

        FakeSource srcCount;
        srcCount.failCount = true;
        names.push_back("stale");
        ok = collectTypeNames(srcCount, ComponentType::Strategy, names);
        if (ok || !names.empty()) {
            ec = fail("getTypeCount error -> false + empty outNames", 61);
            return 0;
        }

        FakeSource srcNext;
        srcNext.rows.push_back(std::make_pair(1, std::string("A")));
        srcNext.failNextId = true;
        names.push_back("stale");
        ok = collectTypeNames(srcNext, ComponentType::Strategy, names);
        if (ok || !names.empty()) {
            ec = fail("getNextId error -> false + empty outNames", 62);
            return 0;
        }

        FakeSource srcThrowCount;
        srcThrowCount.throwAtCount = true;
        names.push_back("stale");
        ok = collectTypeNames(srcThrowCount, ComponentType::Strategy, names);
        if (ok || !names.empty()) {
            ec = fail("getTypeCount exception -> false + empty outNames", 63);
            return 0;
        }

        FakeSource srcThrowName;
        srcThrowName.rows.push_back(std::make_pair(1, std::string("A")));
        srcThrowName.rows.push_back(std::make_pair(2, std::string("B")));
        srcThrowName.throwAtNameId = 2;  // after "A" was collected
        names.push_back("stale");
        ok = collectTypeNames(srcThrowName, ComponentType::Strategy, names);
        printNames("throwName", names);
        if (ok || !names.empty()) {
            ec = fail("getProgramName exception -> false + empty outNames",
                      64);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: failure paths return false with empty names\n");
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

// SEH wrapper - POD-only (no C++ objects with destructors) so __try is legal.
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
                 "=== test_list_items: collectTypeNames (unit) ===\n");

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
                 "\n=== test_list_items complete (exit %d) ===\n", exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}