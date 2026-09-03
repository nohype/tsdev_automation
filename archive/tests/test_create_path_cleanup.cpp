// test_create_path_cleanup.cpp
//
// Unit test for TVS-L.10: a SINGLE ownership decision that covers create-path
// partial-entry cleanup after a successful createEntry for EVERY failure stage
// — saveSource failure AND verifyModule early-return / SEH / OOM — rather than
// leaving cleanup scattered across failure branches.
//
// This is a PURE UNIT TEST. It does NOT load orlib20.dll, does NOT touch the
// TradeStation installation, does NOT construct TsDevLib, and does NOT need a
// 64 MB stack. It calls the implemented ownership decision declared in
// src/create_path_cleanup.h, which is a pure function of (progId, stage).
//
// The implementation in src/create_path_cleanup.cpp is linked into this test.
// The assertions below pin the contract documented in the header.
//
// Why this test exists (historical pre-TVS-L.10 defect):
//   Before TVS-L.10, after createEntry succeeded (ProgId > 0), a partial
//   warehouse entry existed in the in-memory BST. Cleanup of that partial
//   entry on a subsequent failure was not owned by one local path:
//     * saveSource returned false       -> doInsert exit 7, no cleanup.
//     * verifyModule early-return / OOM -> cleanup only incidentally via the
//       !errors.empty() branch inside verifyModule.
//     * verifyModule SEH (ctor / StartCompiling / VerifyModule_New) ->
//       cleanup through that same branch.
//     * verifyModule compile errors    -> the only explicitly-cleaned case.
//   The implemented single owner removes that step-dependent behavior. This
//   test pins the owner contract: for progId > 0 it MUST require cleanup for
//   ALL stages; for progId <= 0 it MUST NOT require cleanup for any stage (no
//   partial entry was created).
//
//   TVS-L.11 extends the stage list with three post-verify failure branches
//   (after successful create+verify): .ell missing on disk, .xml missing on
//   disk, and the L.9 persistence-integrity check failure (Fault A / Invalid
//   Input). doInsert routes ALL three through the SAME cleanupCreatePathFailure
//   owner so the partial warehouse entry + .ell/.xml are removed and the
//   existing structured exit-9 non-success is returned. This test pins that
//   those three new stages are covered by the single owner's contract.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

#include "../src/create_path_cleanup.h"

#include <cstdio>
#include <string>

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

const char* stageStr(tsdev::CreatePathFailureStage s) {
    switch (s) {
        case tsdev::CreatePathFailureStage::SaveSourceFailed:           return "SaveSourceFailed";
        case tsdev::CreatePathFailureStage::VerifyModule_Oom:           return "VerifyModule_Oom";
        case tsdev::CreatePathFailureStage::VerifyModule_SehCtor:       return "VerifyModule_SehCtor";
        case tsdev::CreatePathFailureStage::VerifyModule_SehStartCompiling:
            return "VerifyModule_SehStartCompiling";
        case tsdev::CreatePathFailureStage::VerifyModule_SehVerify:     return "VerifyModule_SehVerify";
        case tsdev::CreatePathFailureStage::VerifyModule_CompileErrors: return "VerifyModule_CompileErrors";
        // TVS-L.11: post-verify failure branches (after successful create+verify).
        case tsdev::CreatePathFailureStage::PostVerify_EllMissing:           return "PostVerify_EllMissing";
        case tsdev::CreatePathFailureStage::PostVerify_XmlMissing:           return "PostVerify_XmlMissing";
        case tsdev::CreatePathFailureStage::PostVerify_PersistenceIntegrity: return "PostVerify_PersistenceIntegrity";
    }
    return "?";
}

int runTests() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_create_path_cleanup: requiresCreatePathCleanup() ===\n");

    // Every failure stage that occurs AFTER a successful createEntry. The
    // single owner MUST require cleanup for all of these when progId > 0.
    // TVS-L.11 extends this list with the three post-verify stages (after
    // successful create+verify): .ell missing, .xml missing, and L.9
    // persistence-integrity failure. These are routed through the SAME
    // cleanupCreatePathFailure owner by doInsert.
    const tsdev::CreatePathFailureStage allStages[] = {
        tsdev::CreatePathFailureStage::SaveSourceFailed,
        tsdev::CreatePathFailureStage::VerifyModule_Oom,
        tsdev::CreatePathFailureStage::VerifyModule_SehCtor,
        tsdev::CreatePathFailureStage::VerifyModule_SehStartCompiling,
        tsdev::CreatePathFailureStage::VerifyModule_SehVerify,
        tsdev::CreatePathFailureStage::VerifyModule_CompileErrors,
        tsdev::CreatePathFailureStage::PostVerify_EllMissing,
        tsdev::CreatePathFailureStage::PostVerify_XmlMissing,
        tsdev::CreatePathFailureStage::PostVerify_PersistenceIntegrity,
    };
    const int numStages = static_cast<int>(sizeof(allStages) / sizeof(allStages[0]));

    int ec = 0;

    // -------------------------------------------------------------------
    // Group 1: createEntry SUCCEEDED (progId > 0).
    //   A partial warehouse entry exists. The single owner MUST require
    //   cleanup for EVERY failure stage. This is the core L.10 invariant:
    //   cleanup is owned by ONE decision covering saveSource failure AND
    //   every verifyModule early-return / SEH / OOM / compile-error mode.
    // -------------------------------------------------------------------
    {
        const int progId = 12345;  // > 0: createEntry succeeded
        for (int i = 0; i < numStages; ++i) {
            tsdev::CreatePathFailureStage s = allStages[i];
            bool req = tsdev::requiresCreatePathCleanup(progId, s);
            std::fprintf(stdout,
                "  [g1] progId=%d stage=%s -> requiresCleanup=%s\n",
                progId, stageStr(s), req ? "true" : "false");
            if (!req) {
                ec = fail(("createEntry succeeded (progId>0) but owner did NOT "
                           "require cleanup for stage " +
                           std::string(stageStr(s))).c_str(), 10 + i);
                return ec;
            }
        }
        std::fprintf(stdout, "    PASS: group 1 (progId>0) all stages require cleanup\n");
    }

    // -------------------------------------------------------------------
    // Group 2: createEntry did NOT succeed (progId <= 0).
    //   No partial entry exists. The single owner MUST NOT require cleanup
    //   for any stage — there is nothing to clean up. This guards against a
    //   implementation that blindly returns true and would attempt to
    //   delete a non-existent / unrelated entry.
    // -------------------------------------------------------------------
    {
        const int progIds[] = {0, -1};
        for (int p = 0; p < 2; ++p) {
            int progId = progIds[p];
            for (int i = 0; i < numStages; ++i) {
                tsdev::CreatePathFailureStage s = allStages[i];
                bool req = tsdev::requiresCreatePathCleanup(progId, s);
                std::fprintf(stdout,
                    "  [g2] progId=%d stage=%s -> requiresCleanup=%s\n",
                    progId, stageStr(s), req ? "true" : "false");
                if (req) {
                    ec = fail(("createEntry did NOT succeed (progId<=0) but owner "
                               "required cleanup for stage " +
                               std::string(stageStr(s))).c_str(), 30 + p * numStages + i);
                    return ec;
                }
            }
        }
        std::fprintf(stdout, "    PASS: group 2 (progId<=0) no stage requires cleanup\n");
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

} // namespace

int main() {
    try {
        int ec = runTests();
        std::fprintf(stdout,
                     "\n=== test_create_path_cleanup complete (exit %d) ===\n", ec);
        if (ec == 0) std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
        else         std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
        return ec;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\n[FATAL] unhandled exception: %s\n", e.what());
        return 99;
    }
}
