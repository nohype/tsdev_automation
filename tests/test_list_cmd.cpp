// tests/test_list_cmd.cpp
//
// Unit tests for the CLI-level `list <type>` output shaping in
// src/tsdev_cli.cpp:
//   - listHumanStdout(names) : the pure human-mode stdout renderer (one
//     stored name per line; an empty result renders NOTHING - empty string).
//   - cliResultToJson(r) "list" success shape: {"op":"list","type":<type>,
//     "success":true,"exit_code":0,"names":[...]} - "names" is ALWAYS
//     present on success (empty array when the category has zero items) and
//     entries keep the given order (no sorting, no dedup).
//   - cliResultToJson(r) "list" failure shape: the standard error object,
//     with NO "names" key.
//   - sortNamesForOutput(names) : THE canonical "list" output ordering,
//     applied exactly once in doList before the names reach the renderers:
//     case-insensitive ASCII comparison (primary key = the name lowercased
//     with ASCII tolower semantics), STABLE ties (names equal ignoring case
//     keep their source order), NO dedup. The renderers above stay
//     order-preserving passthroughs (check 1/3 pin that they do NOT
//     re-sort).
//
// The main() dispatch arg-validation for `list` is wrapped in the
// #ifndef TSDEV_CLI_TEST guard in src/tsdev_cli.cpp and cannot be reached
// from a unit-test TU (and cmdList/runList need a live TsDevLib: TS +
// elevation). doList itself IS driven here (check 12) through the TU-local
// failing-listItems seam above, so no live TsDevLib is constructed. This
// test otherwise covers the pure output-shaping helpers.
//
// This is a PURE UNIT TEST - it does NOT load orlib20.dll, does NOT touch the
// warehouse, and runs under -NoTs. listHumanStdout() and cliResultToJson()
// live inside an anonymous namespace in src/tsdev_cli.cpp (internal linkage),
// so the test #includes the .cpp directly (same TU) to reach them. This pulls
// in tsdev_cli.cpp's main(); the test #defines TSDEV_CLI_TEST before the
// include so main() is excluded by the TSDEV_CLI_TEST guard in
// src/tsdev_cli.cpp.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

// Exclude tsdev_cli.cpp's main() so this test can define its own.
// (main() is wrapped in the #ifndef TSDEV_CLI_TEST guard in tsdev_cli.cpp.)
#define TSDEV_CLI_TEST

// --- Failing-listItems seam -------------------------------------------------
// doList takes TsDevLib& and calls the non-virtual TsDevLib::listItems, so
// no subclass can force its enumerate-failure path without a live
// warehouse. Redirect the token in THIS TU only (same #define-include
// pattern as test_invoke_excepinfo.cpp): the TsDevLib declaration from
// tsdev_lib.h and the doList call site both compile as
// TestSeam_listItems, defined below to fail without touching any
// warehouse state. Production (tsdev_lib.cpp/.lib) is untouched: the only
// code occurrences of the token are the header declaration and the doList
// call site, so nothing else in this TU changes meaning.
// Include the CLI source directly so anonymous-namespace internals
// (listHumanStdout, sortNamesForOutput, cliResultToJson, CliResult,
// ordered_json, doList, kListEnumerateFailure, etc.) are
// visible.
#define listItems TestSeam_listItems
#include "../src/tsdev_cli.cpp"
#undef listItems

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

// Definition of the TU-local failing seam declared above (via the renamed
// TsDevLib member). Clears outNames (documented failure state) and returns
// false. Never touches `this` or any warehouse state, so check 12 can run
// doList against unconstructed storage with no COM, no orlib20, no TS.
namespace tsdev {
bool TsDevLib::TestSeam_listItems(ComponentType /*type*/,
                                  std::vector<std::string>& outNames) {
    outNames.clear();
    return false;
}
} // namespace tsdev

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// Comma-joined names for failure diagnostics ("[a,b,c]").
std::string joinNames(const std::vector<std::string>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += v[i];
    }
    s += "]";
    return s;
}

struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    std::fprintf(stdout,
                 "=== test_list_cmd: list output shaping (unit) ===\n");

    // ------------------------------------------------------------------
    // Check 1: listHumanStdout with several names -> one name per line,
    //   input order preserved (no sorting, no dedup).
    // ------------------------------------------------------------------
    {
        std::vector<std::string> names = { "Zeta", "Alpha", "Alpha" };
        const std::string out = listHumanStdout(names);
        std::fprintf(stdout, "  [1] listHumanStdout(3 names): %s\n",
                     out.c_str());
        if (out != "Zeta\nAlpha\nAlpha\n") {
            ec = fail("check1: names not rendered one-per-line in order", 10);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 2: listHumanStdout with ZERO names -> empty string (an empty
    //   result prints NOTHING).
    // ------------------------------------------------------------------
    {
        std::vector<std::string> names;
        const std::string out = listHumanStdout(names);
        std::fprintf(stdout, "  [2] listHumanStdout(0 names): \"%s\"\n",
                     out.c_str());
        if (!out.empty()) {
            ec = fail("check2: empty result must render NOTHING", 20);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 3: cliResultToJson list success (3 names): exact pinned shape
    //   {"op":"list","type":"strategy","success":true,"exit_code":0,
    //    "names":["Zeta","Alpha","Alpha"]}.
    // ------------------------------------------------------------------
    {
        CliResult r;
        r.op       = "list";
        r.typeStr  = "strategy";
        r.success  = true;
        r.exitCode = 0;
        r.names    = { "Zeta", "Alpha", "Alpha" };

        ordered_json j = cliResultToJson(r);
        std::fprintf(stdout, "  [3] list success (3 names): %s\n",
                     j.dump().c_str());
        const std::string want =
            "{\"op\":\"list\",\"type\":\"strategy\",\"success\":true,"
            "\"exit_code\":0,\"names\":[\"Zeta\",\"Alpha\",\"Alpha\"]}";
        if (j.dump() != want) {
            ec = fail("check3: list success JSON shape mismatch", 30);
            return 0;
        }
        if (j.contains("name")) {
            ec = fail("check3: list JSON must not carry a name key", 31);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 4: cliResultToJson list success (0 names): "names" is STILL
    //   present and is an empty array.
    // ------------------------------------------------------------------
    {
        CliResult r;
        r.op       = "list";
        r.typeStr  = "showme";
        r.success  = true;
        r.exitCode = 0;

        ordered_json j = cliResultToJson(r);
        std::fprintf(stdout, "  [4] list success (0 names): %s\n",
                     j.dump().c_str());
        if (!j.contains("names") || !j["names"].is_array() ||
            !j["names"].empty()) {
            ec = fail("check4: names must be an empty array when zero items",
                      40);
            return 0;
        }
        const std::string want =
            "{\"op\":\"list\",\"type\":\"showme\",\"success\":true,"
            "\"exit_code\":0,\"names\":[]}";
        if (j.dump() != want) {
            ec = fail("check4: empty-names JSON shape mismatch", 41);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 5: cliResultToJson list failure (exit 4): the standard error
    //   object with code/message and NO "names" key. The input errorMessage
    //   below is the pasted production text (what doList stores on
    //   enumerate failure); the assert compares against
    //   kListEnumerateFailure in src/tsdev_cli.cpp (visible via the
    //   same-TU include). The paste is the pin: editing the production
    //   literal without updating the paste (or vice versa) turns this
    //   check red.
    // ------------------------------------------------------------------
    {
        CliResult r;
        r.op       = "list";
        r.typeStr  = "indicator";
        r.success  = false;
        r.exitCode = 4;
        r.errorMessage = "init failed: could not enumerate items";

        ordered_json j = cliResultToJson(r);
        std::fprintf(stdout, "  [5] list failure (exit 4): %s\n",
                     j.dump().c_str());
        if (j.contains("names")) {
            ec = fail("check5: failure JSON must not carry a names key", 50);
            return 0;
        }
        if (!j.contains("error") || !j["error"].is_object()) {
            ec = fail("check5: failure JSON missing error object", 51);
            return 0;
        }
        if (j["error"]["code"].get<std::string>() != "init_failure") {
            ec = fail("check5: error.code != \"init_failure\"", 52);
            return 0;
        }
        if (j["error"]["message"].get<std::string>() !=
            kListEnumerateFailure) {
            ec = fail("check5: error.message mismatch", 53);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 6: sortNamesForOutput mixed case -> case-insensitive ASCII
    //   order with STABLE ties ("Alpha" before "ALPHA": source order kept
    //   for names equal ignoring case).
    // ------------------------------------------------------------------
    {
        std::vector<std::string> names = { "beta", "Alpha", "ALPHA", "zeta" };
        sortNamesForOutput(names);
        const std::vector<std::string> want = { "Alpha", "ALPHA", "beta",
                                                "zeta" };
        std::fprintf(stdout, "  [6] sortNamesForOutput(mixed case): %s\n",
                     joinNames(names).c_str());
        if (names != want) {
            std::fprintf(stderr, "  got %s want %s\n",
                         joinNames(names).c_str(), joinNames(want).c_str());
            ec = fail("check6: mixed-case sort order mismatch", 60);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 7: sortNamesForOutput equal-ignoring-case run -> stays in
    //   source order (stable ties, no reordering among "aB"/"Ab"/"AB").
    // ------------------------------------------------------------------
    {
        std::vector<std::string> names = { "aB", "Ab", "AB" };
        sortNamesForOutput(names);
        const std::vector<std::string> want = { "aB", "Ab", "AB" };
        std::fprintf(stdout, "  [7] sortNamesForOutput(equal-ignoring-case "
                             "run): %s\n",
                     joinNames(names).c_str());
        if (names != want) {
            std::fprintf(stderr, "  got %s want %s\n",
                         joinNames(names).c_str(), joinNames(want).c_str());
            ec = fail("check7: stable tie order not preserved", 70);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 8: sortNamesForOutput duplicates -> preserved (NO dedup);
    //   the three "dup"-keyed names keep source order among themselves.
    // ------------------------------------------------------------------
    {
        std::vector<std::string> names = { "Dup", "dup", "Other", "Dup" };
        sortNamesForOutput(names);
        const std::vector<std::string> want = { "Dup", "dup", "Dup",
                                                "Other" };
        std::fprintf(stdout, "  [8] sortNamesForOutput(duplicates): %s\n",
                     joinNames(names).c_str());
        if (names != want) {
            std::fprintf(stderr, "  got %s want %s\n",
                         joinNames(names).c_str(), joinNames(want).c_str());
            ec = fail("check8: duplicates not preserved in stable order", 80);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 9: sortNamesForOutput on an empty vector and a single element
    //   -> unchanged.
    // ------------------------------------------------------------------
    {
        std::vector<std::string> empty;
        sortNamesForOutput(empty);
        std::vector<std::string> one = { "Solo" };
        sortNamesForOutput(one);
        std::fprintf(stdout, "  [9] sortNamesForOutput(0/1 names): %s %s\n",
                     joinNames(empty).c_str(), joinNames(one).c_str());
        if (!empty.empty()) {
            ec = fail("check9: empty vector must stay empty", 90);
            return 0;
        }
        if (one != std::vector<std::string>{ "Solo" }) {
            ec = fail("check9: single element must stay unchanged", 91);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 10: sortNamesForOutput ASCII punctuation/digit ordering ->
    //   byte order after ASCII-lowercasing ('%' 0x25 < '1' 0x31 < 'a'
    //   0x61): ["%Chg","alpha","10x"] -> ["%Chg","10x","alpha"].
    // ------------------------------------------------------------------
    {
        std::vector<std::string> names = { "%Chg", "alpha", "10x" };
        sortNamesForOutput(names);
        const std::vector<std::string> want = { "%Chg", "10x", "alpha" };
        std::fprintf(stdout, "  [10] sortNamesForOutput(punct/digit): %s\n",
                     joinNames(names).c_str());
        if (names != want) {
            std::fprintf(stderr, "  got %s want %s\n",
                         joinNames(names).c_str(), joinNames(want).c_str());
            ec = fail("check10: ASCII byte order mismatch", 100);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 11: sortNamesForOutput large tie run (40 names equal ignoring
    //   case) framed by distinct keys -> the tie run keeps its source
    //   order (stable), N=42 total. The 40 tie names are distinct
    //   case-variants of "abcdef" (mask bit i uppercases position i) in
    //   descending-mask source order, framed by "zzz"/"aaa" so the input is
    //   unsorted. Exceeds the MSVC insertion-sort cutoff (32): an unstable
    //   std::sort reorders the tie run here (red), stable_sort preserves
    //   it (green). Deterministic data, exact output order asserted.
    // ------------------------------------------------------------------
    {
        const std::string base = "abcdef";
        std::vector<std::string> tie;
        for (int m = 39; m >= 0; --m) {
            std::string s = base;
            for (size_t i = 0; i < s.size(); ++i) {
                if (m & (1 << static_cast<int>(i))) {
                    s[i] = static_cast<char>(s[i] - 'a' + 'A');
                }
            }
            tie.push_back(s);
        }
        std::vector<std::string> names;
        names.push_back("zzz");
        names.insert(names.end(), tie.begin(), tie.end());
        names.push_back("aaa");
        sortNamesForOutput(names);
        std::vector<std::string> want;
        want.push_back("aaa");
        want.insert(want.end(), tie.begin(), tie.end());
        want.push_back("zzz");
        std::fprintf(stdout,
                     "  [11] sortNamesForOutput(40-tie run, N=42): %s\n",
                     joinNames(names).c_str());
        if (names != want) {
            std::fprintf(stderr, "  got %s want %s\n",
                         joinNames(names).c_str(), joinNames(want).c_str());
            ec = fail("check11: large tie run order not preserved", 110);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 12: doList enumerate-failure path through the TU-local
    //   failing-listItems seam -> exit 4 with errorMessage ==
    //   kListEnumerateFailure, and the JSON error.message pins the same
    //   constant. Unlike check 5 (pasted literal through the serializer
    //   only), this drives the real doList: replacing the production
    //   callsite literal with a bypass string turns THIS check red.
    //   The seam never touches `this`, so doList runs against
    //   unconstructed aligned storage: no COM, no orlib20, no warehouse.
    // ------------------------------------------------------------------
    {
        alignas(TsDevLib) unsigned char libStorage[sizeof(TsDevLib)];
        TsDevLib& failingLib = *reinterpret_cast<TsDevLib*>(libStorage);
        CliResult r = doList(failingLib, ComponentType::Strategy, "strategy");
        std::fprintf(stdout, "  [12] doList enumerate failure: exit=%d msg=%s\n",
                     r.exitCode, r.errorMessage.c_str());
        if (r.success) {
            ec = fail("check12: doList must fail when enumeration fails", 120);
            return 0;
        }
        if (r.exitCode != 4) {
            ec = fail("check12: enumerate failure must exit 4", 121);
            return 0;
        }
        if (r.errorMessage != kListEnumerateFailure) {
            ec = fail("check12: errorMessage drifted from constant", 122);
            return 0;
        }
        ordered_json j = cliResultToJson(r);
        if (j.contains("names")) {
            ec = fail("check12: failure JSON must not carry a names key", 123);
            return 0;
        }
        if (j["error"]["message"].get<std::string>() !=
            kListEnumerateFailure) {
            ec = fail("check12: JSON error.message drifted from constant",
                      124);
            return 0;
        }
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
                 "=== test_list_cmd: list output shaping (unit) ===\n");

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
                 "\n=== test_list_cmd complete (exit %d) ===\n", exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}