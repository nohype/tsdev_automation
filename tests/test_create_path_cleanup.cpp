// test_create_path_cleanup.cpp
//
// Unit test (RED-GREEN TDD) for TVS-L.10: a SINGLE ownership decision that
// covers create-path partial-entry cleanup after a successful createEntry for
// EVERY failure stage — saveSource failure AND verifyModule early-return /
// SEH / OOM — instead of cleanup being scattered (only inside verifyModule's
// compile-error branch) or absent (saveSource failure path).
//
// This is a PURE UNIT TEST. It does NOT load orlib20.dll, does NOT touch the
// TradeStation installation, does NOT construct TsDevLib, and does NOT need a
// 64 MB stack. It calls the ownership decision declared in
// src/create_path_cleanup.h, which is a pure function of (progId, stage).
//
// TDD RED step:
//   src/create_path_cleanup.h DECLARES requiresCreatePathCleanup() but there
//   is NO src/create_path_cleanup.cpp yet. This test calls the symbol, so the
//   test executable FAILS TO LINK (unresolved external) until the GREEN step
//   adds the implementation. That link failure IS the RED signal. No
//   assertion can run until the symbol resolves; the test is written so that
//   once the GREEN implementation exists, the assertions below pin the
//   contract documented in the header.
//
// Why this test exists (the defect it targets):
//   After createEntry succeeds (ProgId > 0), a partial warehouse entry exists
//   in the in-memory BST. The create-path can then fail at several points:
//     * saveSource returns false          -> doInsert exit 7, NO cleanup today.
//     * verifyModule early-return / OOM   -> cleanup only incidentally via the
//     * verifyModule SEH (ctor / Start-      !errors.empty() branch INSIDE
//       Compiling / VerifyModule_New)        verifyModule (written for the
//                                             compile-error case, TD-001).
//     * verifyModule compile errors       -> the only explicitly-cleaned case.
//   "Did we clean up?" therefore depends on WHICH step failed and on an
//   incidental property (error-string emptiness), not on a single ownership
//   decision. This test pins the intended single owner: for progId > 0 it
//   MUST require cleanup for ALL stages; for progId <= 0 it MUST NOT
//   require cleanup for any stage (no partial entry was created).
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
// (Until the GREEN step links the owner, the executable does not build, so
//  no exit code is produced — that is the RED.)

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
    //   GREEN implementation that blindly returns true and would attempt to
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
