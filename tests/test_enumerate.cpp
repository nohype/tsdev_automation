// test_enumerate.cpp
//
// Integration test for the current warehouse/API-backed `enumerateRecords`
// behavior of TsDevLib.
//
// TVS-F.5 subtask F5-S2: this test exercises the library-level
// case-insensitive existence-query primitive that the `exists
// --case-insensitive` CLI subcommand wraps.
//
// The test is self-cleaning: it inserts a probe strategy, queries
// enumerateRecords for the existing name in UPPERCASE (expects exists=true,
// verbatim stored name, ProgId > 0) and for a nonexistent name (expects
// exists=false, empty stored name, ProgId == 0), then deletes the probe.
//
// Flow (mirrors tests/test_exists.cpp, per F5-S2 plan):
//   1. Construct TsDevLib (loads orlib20.dll, patches, inits, EDIT control)
//   2. Pre-clean: findExistPos(name, Strategy) -- delete if it already exists
//   3. createEntry(Strategy, name, errCode) -- assert ProgId > 0
//   4. saveSource(Strategy, progId, source) -- assert success
//   5. verifyModule(Strategy, progId) -- assert no errors
//   6. enumerateRecords(Strategy, nameUpper) -- ASSERT exists == true,
//      storedName == kTestName (verbatim, correct case), progId > 0,
//      lookupOk == true
//   7. enumerateRecords(Strategy, "NONEXISTENTNAMEXYZ") -- ASSERT exists ==
//      false, storedName == "", progId == 0, lookupOk == true (a missing
//      item is a clean answer, NOT degraded mode)
//   7a. FIXTURE PRECONDITION (RC-08): verify each step-7b string still
//      appears in some record +0x17 DESCRIPTION field via _LoadMasterRecsEX
//      before using it as a probe; stale fixture => fail 66/67, broken
//      record walk => fail 68 (loud + distinct, never confused with the
//      step-7b "found" failure 65); step 7b is skipped when 7a fails
//   7b. enumerateRecords for live-proven record DESCRIPTION texts -- ASSERT
//      exists == false: a description is never a name (KB Sec. 22/23)
//   8. deleteEntry(Strategy, name) -- cleanup
//   9. Print PASS / FAIL
//
// Exit code: 0 on success, nonzero on failure.
//
// NOTE: The EasyLanguage compiler (invoked via VerifyModule_New) requires a
// large stack, AND the hidden EDIT control created by TsDevLib is a Win32
// window (thread-affine). The reference probe (archive/elevated_test20.cpp) runs its
// ENTIRE flow -- including orlib init and window creation -- in a worker thread
// with a 64 MB stack. We do the same: TsDevLib is constructed AND destructed
// inside the worker thread.

#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace tsdev;

namespace {

// Test fixture constants.
// A unique name minimizes collision risk with other tests. The "zzz_" prefix
// sorts it to the end of the warehouse and signals a test entry.
const char* kTestName = "zzz_test_ci_probe";

// The same name converted to UPPERCASE -- used to verify case-insensitive
// matching via enumerateRecords (the stored name is lowercase/mixed, the
// query is uppercase; the returned storedName must be the verbatim original).
const char* kTestNameUpper = "ZZZ_TEST_CI_PROBE";

// A name that is guaranteed not to exist in the warehouse.
const char* kNonExistentName = "NONEXISTENTNAMEXYZ";

// Live-proven master-record DESCRIPTION texts (docs/KNOWLEDGE_BASE.md
// Sec. 22/23): the 0x10A-byte record family carries NO stored name -- record
// +0x17 is a DESCRIPTION/summary field, and the name index is the ONLY
// authority for name existence. A description string must therefore NEVER
// resolve as an item name. These two strings are the live-proven
// false-positive cases of the pre-fix record-text matching (2026-11-01);
// they are query fixtures only (no item of these names exists).
const char* kDescriptionTextIndicator = "Plots the %R indicator.";
const char* kDescriptionTextFunction =
    "Returns the next label in the cycle for ActivityBar cells";

// Minimal valid EasyLanguage strategy source. In EasyLanguage `{ ... }` is a
// multi-line COMMENT block, so the actual code must be OUTSIDE it. The brace
// text is arbitrary fixture source content only; it is not a metadata
// convention or lookup mechanism. The name and ProgId come from warehouse
// records/APIs. This mirrors the proven source from test_exists.cpp.
const char* kTestSource =
    "{ zzz_test_ci_probe }\r\n"
    "Buy (\"EnterLong\") next bar at Market;\r\n";

// RC-08 fixture-precondition helpers: verify a step-7b probe string still
// appears in some record's +0x17 DESCRIPTION field (KB Sec. 22) by walking
// the master records via _LoadMasterRecsEX@16 -- the same orlib20 export the
// library and the diag record walks use (ord 568; __stdcall, 4 args;
// returns count, allocates count * 0x10A bytes). orlib20.dll is already
// loaded in-process by the TsDevLib instance, so GetModuleHandleA finds it
// with no extra linkage. Read-only: no record is modified, no item is
// created or deleted.
typedef int (__stdcall *LoadMasterRecsExFn)(int category, HGLOBAL* outBuf,
                                            const char* path, int filterMode);

// POD-only SEH wrapper (no C++ objects -- MSVC C2712 forbids them together
// with __try/__except in one function).
struct LoadRecsSehResult { int count; };
bool callLoadMasterRecs(LoadMasterRecsExFn fn, int category, HGLOBAL* outBuf,
                        LoadRecsSehResult* out) {
    out->count = -1;
    __try { out->count = fn(category, outBuf, NULL, /*filterMode=*/1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// Bounded exact-match scan of the +0x17 DESCRIPTION field over `count`
// 0x10A-byte records. The field is a null-terminated string (max observed
// length 60); the bound (0x10A - 0x17) keeps an unterminated field inside
// its own record. On match, the first matching record's ProgId (+0x00
// short) is reported in *outProgId (0 when no match).
bool descriptionPresentInRecords(const char* records, int count,
                                 const char* want, short* outProgId) {
    const int kRecordSize = 0x10A;
    const int kDescOffset = 0x17;
    const int kDescMax = 0x10A - 0x17;
    *outProgId = 0;
    size_t wantLen = std::strlen(want);
    if (wantLen == 0 || wantLen >= (size_t)kDescMax) return false;
    for (int i = 0; i < count; i++) {
        const char* rec = records + (size_t)i * kRecordSize;
        const char* f = rec + kDescOffset;
        size_t j = 0;
        while (j < wantLen && f[j] == want[j]) j++;
        if (j == wantLen && f[j] == '\0') {
            short pid = 0;
            std::memcpy(&pid, rec, sizeof(pid));
            *outProgId = pid;
            return true;
        }
    }
    return false;
}

// Check one fixture string against the live record descriptions of
// `category` (warehouse category: 1 = Function, 2 = Indicator).
// Returns 0 = present (precondition holds), 1 = absent (fixture stale),
// 2 = record walk broken (module/export missing, SEH, lock failure).
int checkDescriptionFixturePresent(int category, const char* want,
                                   short* outProgId) {
    *outProgId = 0;
    HMODULE orlib = GetModuleHandleA("orlib20.dll");
    if (!orlib) return 2;
    LoadMasterRecsExFn loadFn = reinterpret_cast<LoadMasterRecsExFn>(
        GetProcAddress(orlib, "_LoadMasterRecsEX@16"));
    if (!loadFn) return 2;
    HGLOBAL buf = NULL;
    LoadRecsSehResult seh;
    if (!callLoadMasterRecs(loadFn, category, &buf, &seh)) {
        return 2;
    }
    int count = seh.count;
    if (count <= 0 || buf == NULL) {
        if (buf) GlobalFree(buf);
        return 2;
    }
    const char* records = (const char*)GlobalLock(buf);
    if (!records) {
        GlobalFree(buf);
        return 2;
    }
    bool present = descriptionPresentInRecords(records, count, want,
                                              outProgId);
    GlobalUnlock(buf);
    GlobalFree(buf);
    return present ? 0 : 1;
}

// Print a failure banner and return the given code.
int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s\n", msg);
    return code;
}

// Shared state between main and the worker thread.
struct TestState {
    int exitCode = 0;
};

// The full test flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);

    // --- Step 1: Construct TsDevLib ---
    std::fprintf(stdout, "\n[1] Constructing TsDevLib...\n");
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib();
    } catch (const std::exception& e) {
        st->exitCode = fail(e.what(), 10);
        return 0;
    }
    std::fprintf(stdout, "    TsDevLib constructed OK\n");

    // --- Step 2: Pre-clean -- ensure the entry does not already exist ---
    std::fprintf(stdout, "\n[2] Pre-clean: findExistPos(\"%s\", Strategy)...\n",
                 kTestName);
    int existing = lib->findExistPos(kTestName, ComponentType::Strategy);
    std::fprintf(stdout, "    findExistPos = %d\n", existing);
    if (existing > 0) {
        std::fprintf(stdout,
                     "    Entry already exists (ProgId=%d) -- deleting...\n",
                     existing);
        bool deleted = lib->deleteEntry(ComponentType::Strategy, kTestName);
        std::fprintf(stdout, "    deleteEntry = %s\n",
                     deleted ? "true" : "false");
    }

    // --- Step 3: createEntry ---
    std::fprintf(stdout, "\n[3] createEntry(Strategy, \"%s\")...\n",
                 kTestName);
    int errCode = 0;
    int progId = lib->createEntry(ComponentType::Strategy, kTestName, errCode);
    std::fprintf(stdout, "    createEntry = %d, errCode = 0x%X\n",
                 progId, errCode);
    if (progId <= 0) {
        st->exitCode = fail("createEntry returned ProgId <= 0", 20);
        delete lib;
        return 0;
    }

    // --- Step 4: saveSource ---
    std::fprintf(stdout, "\n[4] saveSource(Strategy, %d, source)...\n",
                 progId);
    bool saved = lib->saveSource(ComponentType::Strategy, progId,
                                 kTestSource);
    std::fprintf(stdout, "    saveSource = %s\n", saved ? "true" : "false");
    if (!saved) {
        st->exitCode = fail("saveSource returned false", 30);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }

    // --- Step 5: verifyModule (compile + persist) ---
    std::fprintf(stdout, "\n[5] verifyModule(Strategy, %d)...\n", progId);
    std::string errors =
        lib->verifyModule(ComponentType::Strategy, progId);
    if (errors.empty()) {
        std::fprintf(stdout, "    verifyModule: no errors (compilation OK)\n");
    } else {
        std::fprintf(stderr, "    verifyModule errors:\n%s\n",
                     errors.c_str());
        st->exitCode = fail("verifyModule returned compilation errors", 40);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }

    // --- Step 6: enumerateRecords for the EXISTING name (UPPERCASE query) ---
    // Asserts case-insensitive match: querying "ZZZ_TEST_CI_PROBE" must find
    // the item stored as "zzz_test_ci_probe" (verbatim stored name returned),
    // with exists == true and progId > 0.
    std::fprintf(stdout,
                 "\n[6] enumerateRecords(Strategy, \"%s\") -- "
                 "asserting case-insensitive MATCH...\n",
                 kTestNameUpper);
    EnumerateResult result =
        lib->enumerateRecords(ComponentType::Strategy, kTestNameUpper);
    std::fprintf(stdout,
                 "    enumerateRecords: exists=%d, storedName=\"%s\", "
                 "progId=%d\n",
                 result.exists ? 1 : 0, result.storedName.c_str(),
                 result.progId);
    if (!result.exists) {
        st->exitCode = fail(
            "enumerateRecords returned exists == false for an existing item "
            "(queried case-insensitively)",
            50);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }
    if (result.progId <= 0) {
        st->exitCode = fail(
            "enumerateRecords returned progId <= 0 for an existing item",
            51);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }
    // The stored name must be the VERBATIM (correctly-cased) stored name,
    // NOT the uppercase query. This is the key case-insensitive invariant.
    if (result.storedName != kTestName) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "enumerateRecords storedName mismatch: expected \"%s\" "
                      "(verbatim), got \"%s\"",
                      kTestName, result.storedName.c_str());
        st->exitCode = fail(buf, 52);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }
    if (!result.lookupOk) {
        st->exitCode = fail(
            "enumerateRecords reported degraded lookup (lookupOk == false) "
            "for a healthy found result",
            53);
        lib->deleteEntry(ComponentType::Strategy, kTestName);
        delete lib;
        return 0;
    }
    std::fprintf(stdout,
                 "    PASS: case-insensitive match found (storedName=\"%s\", "
                 "progId=%d)\n",
                 result.storedName.c_str(), result.progId);

    // --- Step 7: enumerateRecords for a NONEXISTENT name -- ASSERT not found ---
    // Fail-loud note (tri-state contract, docs/KNOWLEDGE_BASE.md Sec. 22/23):
    // this fixture covers ONLY the honest missing-item shape -- the BST search
    // answers "not found" AND stores no node (a consistent answer), which must
    // stay a clean exists=false, lookupOk=true. A contradictory-but-SEH-free
    // BST answer ("found" with a null node, or "not found" with a non-null
    // node) is broken machinery and must yield lookupOk=false, never this
    // clean shape; it is classified in TsDevLib::enumerateRecords()
    // (src/tsdev_lib.cpp, the callBstSearchWrapper boundary) and cannot be
    // provoked from a healthy warehouse, so it is verified by code inspection
    // there (the pure seam tests/test_name_index_outcome.cpp covers the
    // outcome contract it folds into).
    std::fprintf(stdout,
                 "\n[7] enumerateRecords(Strategy, \"%s\") -- "
                 "asserting item does NOT exist...\n",
                 kNonExistentName);
    EnumerateResult notFound =
        lib->enumerateRecords(ComponentType::Strategy, kNonExistentName);
    std::fprintf(stdout,
                 "    enumerateRecords: exists=%d, storedName=\"%s\", "
                 "progId=%d\n",
                 notFound.exists ? 1 : 0, notFound.storedName.c_str(),
                 notFound.progId);
    if (notFound.exists) {
        st->exitCode = fail(
            "enumerateRecords returned exists == true for a nonexistent item",
            60);
        // Still clean up the probe entry below.
    } else if (notFound.progId != 0) {
        st->exitCode = fail(
            "enumerateRecords returned nonzero progId for a nonexistent item",
            61);
    } else if (!notFound.storedName.empty()) {
        st->exitCode = fail(
            "enumerateRecords returned non-empty storedName for a nonexistent "
            "item",
            62);
    } else if (!notFound.lookupOk) {
        // A missing item is a CLEAN answer (lookupOk == true). lookupOk ==
        // false here would mean the lookup system is broken and the caller
        // must surface an error, not a silent "not found".
        st->exitCode = fail(
            "enumerateRecords reported degraded lookup (lookupOk == false) "
            "for a healthy not-found result",
            63);
    } else {
        std::fprintf(stdout,
                     "    PASS: nonexistent item not found (exists=false, "
                     "storedName=\"\", progId=0)\n");
    }

    // --- Step 7a: fixture-precondition check (RC-08) ---
    // The step-7b probes are live dictionary DESCRIPTION strings: they only
    // guard the false-positive root cause while they still appear in some
    // record's +0x17 DESCRIPTION field. A dictionary update could silently
    // remove them and turn 7b into a vacuous check, so assert the
    // precondition here, BEFORE using them as probes. Failure is loud and
    // distinct: 66/67 = fixture stale (the dictionary no longer carries the
    // text -- NOT a lookup failure), 68 = record walk itself broken
    // (precondition unverifiable). Step 7b is skipped when 7a fails.
    std::fprintf(stdout,
                 "\n[7a] Verifying description fixtures still present in "
                 "live record descriptions...\n");
    lib->refreshCache();
    struct FixtureCheck {
        const char*   text;
        int           category;
        const char*   label;
        int           staleCode;
    };
    const FixtureCheck kFixtureChecks[] = {
        {kDescriptionTextIndicator, getCategory(ComponentType::Indicator),
         "Indicator", 66},
        {kDescriptionTextFunction, getCategory(ComponentType::Function),
         "Function", 67},
    };
    for (const FixtureCheck& fc : kFixtureChecks) {
        short holderProgId = 0;
        int pre = checkDescriptionFixturePresent(fc.category, fc.text,
                                                &holderProgId);
        if (pre == 0) {
            std::fprintf(stdout,
                         "    PRECONDITION OK: %s description still present "
                         "(holder ProgId=%d)\n",
                         fc.label, (int)holderProgId);
        } else if (pre == 1) {
            char msg[512];
            std::snprintf(msg, sizeof(msg),
                          "description fixture is STALE (not found in any "
                          "%s record description field): \"%s\" -- the "
                          "dictionary changed; step 7b would be vacuous",
                          fc.label, fc.text);
            st->exitCode = fail(msg, fc.staleCode);
            break;
        } else {
            st->exitCode = fail(
                "record-walk precondition check failed (orlib20 handle / "
                "_LoadMasterRecsEX@16 / SEH / lock) -- fixture presence "
                "unverifiable",
                68);
            break;
        }
    }

    // --- Step 7b: record DESCRIPTION texts must NEVER resolve as names ---
    // Skipped when step 7a failed (a stale/unverifiable fixture would make
    // this probe vacuous or misleading); the 7a code already singles out
    // the cause.
    if (st->exitCode == 0) {
    // Master records carry NO stored name: record +0x17 is a
    // DESCRIPTION/summary field (docs/KNOWLEDGE_BASE.md Sec. 22/23). The two
    // strings below are live-proven description texts of dictionary items;
    // the pre-fix record-text matching falsely reported them as existing
    // items. The name-index lookup must return a clean not-found for them.
    std::fprintf(stdout,
                 "\n[7b] enumerateRecords for record DESCRIPTION texts -- "
                 "asserting they do NOT resolve as names...\n");
    struct DescriptionProbe {
        const char*   text;
        ComponentType type;
        const char*   typeLabel;
    };
    const DescriptionProbe kDescriptionProbes[] = {
        {kDescriptionTextIndicator, ComponentType::Indicator, "Indicator"},
        {kDescriptionTextFunction, ComponentType::Function, "Function"},
    };
    for (const DescriptionProbe& probe : kDescriptionProbes) {
        EnumerateResult desc = lib->enumerateRecords(probe.type, probe.text);
        std::fprintf(stdout,
                     "    enumerateRecords(%s, \"%s\"): exists=%d, "
                     "storedName=\"%s\", progId=%d\n",
                     probe.typeLabel, probe.text, desc.exists ? 1 : 0,
                     desc.storedName.c_str(), desc.progId);
        if (!desc.lookupOk) {
            st->exitCode = fail(
                "enumerateRecords reported degraded lookup (lookupOk == "
                "false) for a description-text query",
                64);
        } else if (desc.exists || !desc.storedName.empty() || desc.progId != 0) {
            st->exitCode = fail(
                "enumerateRecords reported a record DESCRIPTION text as an "
                "existing item (the name index is the only name authority)",
                65);
        } else {
            std::fprintf(stdout,
                         "    PASS: description text did not resolve as a "
                         "name\n");
        }
    }
    } // end Step 7b guard (skipped when Step 7a failed)

    // --- Step 8: Cleanup -- delete the probe entry ---
    std::fprintf(stdout, "\n[8] Cleanup: deleteEntry(Strategy, \"%s\")...\n",
                 kTestName);
    bool deleted = lib->deleteEntry(ComponentType::Strategy, kTestName);
    std::fprintf(stdout, "    deleteEntry = %s\n",
                 deleted ? "true" : "false");
    if (!deleted) {
        st->exitCode = fail("cleanup deleteEntry failed", 70);
    }

    // --- Step 9: Result ---
    if (st->exitCode == 0) {
        std::fprintf(stdout,
                     "\nPASS: enumerateRecords correctly reports "
                     "case-insensitive existence and nonexistence\n");
    }

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_enumerate: enumerateRecords case-insensitive query "
                 "(TVS-F.5) ===\n");
    std::fprintf(stdout, "Strategy name: %s\n", kTestName);
    std::fprintf(stdout, "Upper query  : %s\n", kTestNameUpper);
    std::fprintf(stdout, "Nonexistent  : %s\n", kNonExistentName);

    // Run the entire test on a worker thread with a 64 MB stack. The
    // EasyLanguage compiler needs far more than the default 1 MB, and the
    // hidden EDIT control is thread-affine so TsDevLib must live on this
    // thread.
    TestState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runTest, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    std::fprintf(stdout, "\n=== test_enumerate complete (exit %d) ===\n",
                 st.exitCode);
    return st.exitCode;
}
