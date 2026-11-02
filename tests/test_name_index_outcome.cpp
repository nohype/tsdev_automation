// test_name_index_outcome.cpp
//
// Pure unit test for the name-index lookup outcome contract (the tri-state
// handling behind EnumerateResult). The warehouse name index is the ONLY
// authority for name existence (docs/KNOWLEDGE_BASE.md Sec. 22/23):
//   - a MISSING item  -> lookupOk == true,  exists == false (clean answer)
//   - a BROKEN lookup -> lookupOk == false (FAILURE to surface as an error,
//     never a silent "not found")
//
// This test exercises the exact helper used by TsDevLib::enumerateRecords()
// through the test-only seam makeNameIndexOutcomeForTest; it does not
// construct TsDevLib, load orlib20.dll, or touch a TradeStation
// installation.
//
// Seam boundary: the classifier receives the already-combined lookup facts.
// The raw BST answer (search result + node pointer) is combined in
// TsDevLib::enumerateRecords() at the callBstSearchWrapper boundary: a
// contradictory-but-SEH-free pair ("found" with no node, or "not found" with
// a node) is folded to mechanismOk == false there, so it reaches this seam
// as the broken-machinery shape covered by case [2]. The honest not-found
// pair ("not found" with no node) reaches case [3]. The contradictory pairs
// themselves cannot be expressed through this seam (it carries no raw BST
// answer) -- see the fail-loud note at step 7 of tests/test_enumerate.cpp.

#include "../src/tsdev_lib_test_hooks.h"

#include <cstdio>
#include <string>

using namespace tsdev;

namespace {

int fail(const char* message, int code) {
    std::fprintf(stderr, "[FAIL] %s (code=%d)\n", message, code);
    return code;
}

bool isCleanNotFound(const EnumerateResult& r) {
    return r.lookupOk && !r.exists && r.storedName.empty() && r.progId == 0;
}

bool isBrokenLookup(const EnumerateResult& r) {
    return !r.lookupOk && !r.exists && r.storedName.empty() && r.progId == 0;
}

bool isFound(const EnumerateResult& r, const std::string& name, int progId) {
    return r.lookupOk && r.exists && r.storedName == name && r.progId == progId;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_name_index_outcome: lookup outcome contract "
                 "(unit) ===\n");

    // [1] Broken machinery is an ERROR even when a name was found: the
    //     broken state must never masquerade as a lookup answer.
    {
        EnumerateResult r = tsdev::makeNameIndexOutcomeForTest(
            /*mechanismOk=*/false, /*found=*/true, "Some Name", 42);
        if (!isBrokenLookup(r)) {
            return fail("broken machinery with a found node must yield "
                        "lookupOk=false and no answer",
                        10);
        }
        std::fprintf(stdout,
                     "  [1] PASS: broken machinery is an error, not an "
                     "answer\n");
    }

    // [2] Broken machinery is NEVER a silent "not found": the missing-item
    //     shape (clean exists=false) must not be produced.
    {
        EnumerateResult r = tsdev::makeNameIndexOutcomeForTest(
            /*mechanismOk=*/false, /*found=*/false, "", 0);
        if (!isBrokenLookup(r)) {
            return fail("broken machinery without a match must yield "
                        "lookupOk=false, never a silent not-found",
                        20);
        }
        std::fprintf(stdout,
                     "  [2] PASS: broken machinery is not a silent "
                     "not-found\n");
    }

    // [3] Healthy not-found: a missing item is a clean answer.
    {
        EnumerateResult r = tsdev::makeNameIndexOutcomeForTest(
            /*mechanismOk=*/true, /*found=*/false, "", 0);
        if (!isCleanNotFound(r)) {
            return fail("healthy no-match must yield lookupOk=true with "
                        "exists=false",
                        30);
        }
        std::fprintf(stdout,
                     "  [3] PASS: missing item is a clean not-found\n");
    }

    // [4] Healthy found: verbatim stored name and the FindExistPos ProgId.
    {
        EnumerateResult r = tsdev::makeNameIndexOutcomeForTest(
            /*mechanismOk=*/true, /*found=*/true, "Real Name", 4242);
        if (!isFound(r, "Real Name", 4242)) {
            return fail("healthy match must yield exists=true with the "
                        "verbatim name and ProgId",
                        40);
        }
        std::fprintf(stdout,
                     "  [4] PASS: found item carries verbatim name + "
                     "ProgId\n");
    }

    // [5] A found name with a non-positive FindExistPos result keeps the
    //     established contract (exists=1, progId=0): the lookup answered,
    //     only the ProgId channel is unavailable.
    {
        EnumerateResult r = tsdev::makeNameIndexOutcomeForTest(
            /*mechanismOk=*/true, /*found=*/true, "Real Name", -1);
        if (!isFound(r, "Real Name", 0)) {
            return fail("found name with non-positive ProgId must yield "
                        "exists=true, progId=0",
                        50);
        }
        std::fprintf(stdout,
                     "  [5] PASS: found name with non-positive ProgId "
                     "reports progId=0\n");
    }

    std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    return 0;
}