// test_delete_disk_cleanup.cpp
//
// Unit test (GREEN) for TVS-L.12: the SINGLE decision that classifies
// whether a DeleteFileA failure on a delete-path disk artifact (.ell / .xml)
// must FAIL the overall deleteEntry Result, instead of the previous behavior
// where every non-ERROR_FILE_NOT_FOUND DeleteFileA failure was silently
// demoted to a stderr WARNING while deleteEntry returned true (success).
//
// This is a PURE UNIT TEST. It does NOT load orlib20.dll, does NOT touch the
// TradeStation installation, does NOT construct TsDevLib, and does NOT need a
// 64 MB stack. It calls the decision declared in src/delete_disk_cleanup.h,
// which is a pure function of (lastError, kind).
//
// TDD history:
//   The RED step declared deleteDiskCleanupErrorFailsResult() in
//   src/delete_disk_cleanup.h with no src/delete_disk_cleanup.cpp, so this
//   test executable FAILED TO LINK (unresolved external) — that link failure
//   was the RED signal. The GREEN step added src/delete_disk_cleanup.cpp
//   implementing the pure function; this test now links and the assertions
//   below pin the contract documented in the header.
//
// Why this test exists (the defect it targets):
//   deleteEntry step 5 deletes elmodule_<whType>_<progId>.ell and .xml via
//   DeleteFileA. Before L.12 it only emitted a stderr WARNING for any error
//   other than ERROR_FILE_NOT_FOUND, then returned true UNCONDITIONALLY. So a
//   real removal failure (e.g. ERROR_ACCESS_DENIED = 5: file present but
//   read-only / locked / ACL-denied) was treated as success and a stale
//   .ell/.xml was left on disk. This test pins the single decision:
//   ERROR_FILE_NOT_FOUND (and ERROR_SUCCESS) MUST NOT fail the delete Result;
//   ERROR_ACCESS_DENIED (and any other nonzero error) MUST fail it - for BOTH
//   the .ell and the .xml artifact.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

#include "../src/delete_disk_cleanup.h"

#include <cstdio>
#include <string>

namespace {

// Win32 error codes as raw numeric constants with the intended symbol named,
// per the project's no-hack rule (avoid pulling <windows.h> into this pure
// unit test; the header uses `unsigned long` = DWORD on Win32 for the same
// reason). These are stable Win32 values, not magic numbers.
const unsigned long kErrorSuccess        = 0UL; // ERROR_SUCCESS
const unsigned long kErrorFileNotFound   = 2UL; // ERROR_FILE_NOT_FOUND
const unsigned long kErrorAccessDenied   = 5UL; // ERROR_ACCESS_DENIED
// A representative "other nonzero" removal failure used to pin the fail-safe
// branch. ERROR_SHARING_VIOLATION (32) is a realistic DeleteFileA failure
// (file open by another process) distinct from access-denied.
const unsigned long kErrorSharingViolation = 32UL; // ERROR_SHARING_VIOLATION

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

const char* kindStr(tsdev::DeleteDiskArtifactKind k) {
    switch (k) {
        case tsdev::DeleteDiskArtifactKind::Ell: return "Ell";
        case tsdev::DeleteDiskArtifactKind::Xml: return "Xml";
    }
    return "?";
}

const char* errName(unsigned long e) {
    switch (e) {
        case 0UL:  return "ERROR_SUCCESS";
        case 2UL:  return "ERROR_FILE_NOT_FOUND";
        case 5UL:  return "ERROR_ACCESS_DENIED";
        case 32UL: return "ERROR_SHARING_VIOLATION";
        default:   return "(other)";
    }
}

// Expected contract from the header:
//   ERROR_SUCCESS        -> false (must NOT fail the delete Result)
//   ERROR_FILE_NOT_FOUND -> false (must NOT fail - idempotent absence)
//   ERROR_ACCESS_DENIED  -> true  (MUST fail the delete Result)
//   any other nonzero    -> true  (MUST fail - fail safe)
bool expectedFails(unsigned long lastError) {
    if (lastError == kErrorSuccess)        return false;
    if (lastError == kErrorFileNotFound)   return false;
    return true;  // ERROR_ACCESS_DENIED, ERROR_SHARING_VIOLATION, anything else
}

int runTests() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_delete_disk_cleanup: "
                 "deleteDiskCleanupErrorFailsResult() ===\n");

    // Both delete-path disk artifacts are covered: the same rule MUST hold
    // for the .ell and the .xml DeleteFileA call. "Both ELL and XML
    // considered" is pinned by exercising every error code for BOTH kinds.
    const tsdev::DeleteDiskArtifactKind allKinds[] = {
        tsdev::DeleteDiskArtifactKind::Ell,
        tsdev::DeleteDiskArtifactKind::Xml,
    };
    const int numKinds =
        static_cast<int>(sizeof(allKinds) / sizeof(allKinds[0]));

    // The error codes that distinguish benign absence from real failure.
    const unsigned long allErrors[] = {
        kErrorSuccess,
        kErrorFileNotFound,
        kErrorAccessDenied,
        kErrorSharingViolation,
    };
    const int numErrors =
        static_cast<int>(sizeof(allErrors) / sizeof(allErrors[0]));

    int ec = 0;

    // -------------------------------------------------------------------
    // Group 1: ERROR_FILE_NOT_FOUND is SUCCESS (must NOT fail the Result).
    //   This is the ONE case today's deleteEntry already handles correctly
    //   (it suppresses the WARNING and returns true). The decision MUST
    //   preserve it: an already-absent artifact is an idempotent delete, not
    //   a failure. Covered for BOTH ELL and XML.
    // -------------------------------------------------------------------
    {
        const unsigned long e = kErrorFileNotFound;
        for (int k = 0; k < numKinds; ++k) {
            tsdev::DeleteDiskArtifactKind kind = allKinds[k];
            bool fails = tsdev::deleteDiskCleanupErrorFailsResult(e, kind);
            std::fprintf(stdout,
                "  [g1] kind=%s err=%s(%lu) -> failsResult=%s "
                "(expected false)\n",
                kindStr(kind), errName(e), e,
                fails ? "true" : "false");
            if (fails) {
                ec = fail((std::string("ERROR_FILE_NOT_FOUND must NOT fail "
                                       "the delete Result for ") +
                           kindStr(kind) + " but the decision returned true")
                          .c_str(), 10 + k);
                return ec;
            }
        }
        std::fprintf(stdout,
            "    PASS: group 1 - ERROR_FILE_NOT_FOUND is success for "
            "both ELL and XML\n");
    }

    // -------------------------------------------------------------------
    // Group 2: ERROR_ACCESS_DENIED (5) is FAILURE (MUST fail the Result).
    //   This is the core defect: today's deleteEntry logs a WARNING and
    //   returns true, leaving a stale .ell/.xml on disk. The decision MUST
    //   classify it as a real failure so the delete Result fails. Covered
    //   for BOTH ELL and XML.
    // -------------------------------------------------------------------
    {
        const unsigned long e = kErrorAccessDenied;
        for (int k = 0; k < numKinds; ++k) {
            tsdev::DeleteDiskArtifactKind kind = allKinds[k];
            bool fails = tsdev::deleteDiskCleanupErrorFailsResult(e, kind);
            std::fprintf(stdout,
                "  [g2] kind=%s err=%s(%lu) -> failsResult=%s "
                "(expected true)\n",
                kindStr(kind), errName(e), e,
                fails ? "true" : "false");
            if (!fails) {
                ec = fail((std::string("ERROR_ACCESS_DENIED MUST fail the "
                                       "delete Result for ") +
                           kindStr(kind) +
                           " but the decision returned false "
                           "(disk-cleanup failure treated as success)")
                          .c_str(), 20 + k);
                return ec;
            }
        }
        std::fprintf(stdout,
            "    PASS: group 2 - ERROR_ACCESS_DENIED is failure for "
            "both ELL and XML\n");
    }

    // -------------------------------------------------------------------
    // Group 3: ERROR_SUCCESS (DeleteFileA returned TRUE) is SUCCESS.
    //   The artifact was removed; the delete Result MUST NOT fail. Covered
    //   for BOTH ELL and XML. (Guards against a GREEN implementation that
    //   blindly returns true.)
    // -------------------------------------------------------------------
    {
        const unsigned long e = kErrorSuccess;
        for (int k = 0; k < numKinds; ++k) {
            tsdev::DeleteDiskArtifactKind kind = allKinds[k];
            bool fails = tsdev::deleteDiskCleanupErrorFailsResult(e, kind);
            std::fprintf(stdout,
                "  [g3] kind=%s err=%s(%lu) -> failsResult=%s "
                "(expected false)\n",
                kindStr(kind), errName(e), e,
                fails ? "true" : "false");
            if (fails) {
                ec = fail((std::string("ERROR_SUCCESS must NOT fail the "
                                       "delete Result for ") +
                           kindStr(kind) + " but the decision returned true")
                          .c_str(), 30 + k);
                return ec;
            }
        }
        std::fprintf(stdout,
            "    PASS: group 3 - ERROR_SUCCESS is success for "
            "both ELL and XML\n");
    }

    // -------------------------------------------------------------------
    // Group 4: any other nonzero removal failure is FAILURE (fail safe).
    //   ERROR_SHARING_VIOLATION (32) is a realistic DeleteFileA failure
    //   (file open by another process). The decision MUST fail the delete
    //   Result rather than silently treat it as success. Covered for BOTH
    //   ELL and XML. (Pins the fail-safe branch for unknown errors.)
    // -------------------------------------------------------------------
    {
        const unsigned long e = kErrorSharingViolation;
        for (int k = 0; k < numKinds; ++k) {
            tsdev::DeleteDiskArtifactKind kind = allKinds[k];
            bool fails = tsdev::deleteDiskCleanupErrorFailsResult(e, kind);
            std::fprintf(stdout,
                "  [g4] kind=%s err=%s(%lu) -> failsResult=%s "
                "(expected true)\n",
                kindStr(kind), errName(e), e,
                fails ? "true" : "false");
            if (!fails) {
                ec = fail((std::string("a nonzero removal failure (") +
                           errName(e) + ") MUST fail the delete Result for " +
                           kindStr(kind) +
                           " but the decision returned false")
                          .c_str(), 40 + k);
                return ec;
            }
        }
        std::fprintf(stdout,
            "    PASS: group 4 - other nonzero error is failure for "
            "both ELL and XML\n");
    }

    // -------------------------------------------------------------------
    // Group 5: full matrix sweep - the decision MUST match `expectedFails`
    //   for every (error, kind) combination. This is the exhaustive guard
    //   that the four targeted groups above are summarized from.
    // -------------------------------------------------------------------
    {
        for (int k = 0; k < numKinds; ++k) {
            tsdev::DeleteDiskArtifactKind kind = allKinds[k];
            for (int i = 0; i < numErrors; ++i) {
                unsigned long e = allErrors[i];
                bool fails = tsdev::deleteDiskCleanupErrorFailsResult(e, kind);
                bool want = expectedFails(e);
                std::fprintf(stdout,
                    "  [g5] kind=%s err=%s(%lu) -> failsResult=%s "
                    "(expected %s)\n",
                    kindStr(kind), errName(e), e,
                    fails ? "true" : "false",
                    want ? "true" : "false");
                if (fails != want) {
                    ec = fail((std::string("matrix mismatch for kind=") +
                               kindStr(kind) + " err=" + errName(e) +
                               " got=" + (fails ? "true" : "false") +
                               " want=" + (want ? "true" : "false"))
                              .c_str(), 50 + k * numErrors + i);
                    return ec;
                }
            }
        }
        std::fprintf(stdout,
            "    PASS: group 5 - full (error x kind) matrix matches "
            "contract\n");
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

} // namespace

int main() {
    try {
        int ec = runTests();
        std::fprintf(stdout,
                     "\n=== test_delete_disk_cleanup complete (exit %d) ===\n",
                     ec);
        if (ec == 0) std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
        else         std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
        return ec;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\n[FATAL] unhandled exception: %s\n", e.what());
        return 99;
    }
}
