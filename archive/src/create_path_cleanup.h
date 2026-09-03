// create_path_cleanup.h
//
// Single-owner create-path partial-entry cleanup API (TVS-L.10, GREEN).
//
// This header DECLARES the ONE ownership point that decides whether a partial
// warehouse entry created by a successful createEntry must be cleaned up when
// the create-path subsequently fails. It exists so that a unit test
// (tests/test_create_path_cleanup.cpp) and the production CLI can share ONE
// definition of "the create-path failed after createEntry succeeded, so the
// partial entry must be removed", instead of cleanup being scattered across
// the create-path's failure branches.
//
// STATUS: IMPLEMENTED. src/create_path_cleanup.cpp defines the pure decision
// used by the production CLI and by tests/test_create_path_cleanup.cpp. The
// unit test links the implementation through tsdev_lib; the contract is stated
// below.
//
// Why a single owner is needed (historical pre-TVS-L.10 defect):
//   Before TVS-L.10, after createEntry succeeded (ProgId > 0, a partial entry
//   existed in the in-memory warehouse BST), cleanup of that partial entry on
//   subsequent failure was NOT owned by one local path:
//     * saveSource failure (doInsert exit 7): NO cleanup at all — the partial
//       entry was orphaned in the BST.
//     * verifyModule early-return / SEH / OOM paths: cleanup only happened
//       incidentally because every early-return yielded a non-empty error
//       string and the existing auto-cleanup branch inside verifyModule keyed
//       off `!errors.empty()`. That branch was written for the compile-error
//       case (TD-001); it was not an explicit owner for the SEH/OOM/early-return
//       failure modes, and it lived INSIDE verifyModule rather than at the
//       create-path level.
//   The historical result was that "did we clean up the partial entry?"
//   depended on WHICH step failed and on an incidental property (error-string
//   emptiness) of the failing step, not on a single ownership decision. The
//   implemented API is the single owner that covers ALL failure stages after a
//   successful createEntry: saveSource failure AND every verifyModule failure
//   mode (OOM, SEH in ctor / StartCompiling / VerifyModule_New, and compile
//   errors).
//
// Implemented contract (pinned by the unit test):
//   * progId <= 0  -> createEntry did NOT succeed, no partial entry exists:
//                     requiresCreatePathCleanup returns false for every stage.
//   * progId >  0  -> createEntry DID succeed, a partial entry exists:
//                     requiresCreatePathCleanup returns true for EVERY stage
//                     listed below (SaveSourceFailed, VerifyModule_Oom,
//                     VerifyModule_SehCtor, VerifyModule_SehStartCompiling,
//                     VerifyModule_SehVerify, VerifyModule_CompileErrors,
//                     and the TVS-L.11 post-verify stages
//                     PostVerify_EllMissing, PostVerify_XmlMissing,
//                     PostVerify_PersistenceIntegrity).
//   The decision is a pure function of (progId, stage); it does not touch
//   orlib20, the warehouse, or the filesystem, so it can be unit-tested with
//   no TS dependency (matching the TVS-L.9 pure-unit-test pattern).

#ifndef TSDEV_CREATE_PATH_CLEANUP_H
#define TSDEV_CREATE_PATH_CLEANUP_H

namespace tsdev {

// The stage the create-path reached when it failed. The precondition for every
// stage is that createEntry already succeeded (ProgId > 0, partial warehouse
// entry now exists in the in-memory BST). The stages enumerate the failure
// points AFTER that success where the partial entry must be removed.
enum class CreatePathFailureStage {
    // saveSource returned false after a successful createEntry.
    // (Historical pre-TVS-L.10 doInsert exit 7 behavior: no cleanup, partial
    // entry orphaned.)
    SaveSourceFailed,

    // verifyModule returned early because ordinal 66 (operator_new(0x58))
    // failed (out of memory). Historical pre-TVS-L.10 behavior: cleanup fired
    // only incidentally via the !errors.empty() branch inside verifyModule.
    VerifyModule_Oom,

    // verifyModule returned early because the CELCompiler constructor raised
    // an SEH exception. Historical pre-TVS-L.10 behavior: incidental cleanup
    // via !errors.empty().
    VerifyModule_SehCtor,

    // verifyModule returned early because StartCompilingProcess raised an SEH
    // exception. Historical pre-TVS-L.10 behavior: incidental cleanup via
    // !errors.empty().
    VerifyModule_SehStartCompiling,

    // verifyModule returned early because VerifyModule_New raised an SEH
    // exception. Historical pre-TVS-L.10 behavior: incidental cleanup via
    // !errors.empty().
    VerifyModule_SehVerify,

    // verifyModule completed and reported compile errors. Historical
    // pre-TVS-L.10 behavior: this was the ONLY case the code cleaned up
    // explicitly (TD-001 auto-cleanup branch inside verifyModule).
    VerifyModule_CompileErrors,

    // TVS-L.11: post-verify failure branches. These occur AFTER a successful
    // createEntry AND a successful verifyModule (compile/link reported no
    // errors), but BEFORE the create-path can report success. A partial
    // warehouse entry still exists and must be removed. The existing
    // requiresCreatePathCleanup() pure function already returns true for
    // progId > 0 regardless of stage, so these enumerators require NO logic
    // change — they extend the single owner's coverage to the post-verify
    // existence + persistence-integrity failures, routed through the SAME
    // cleanupCreatePathFailure owner (deleteEntry(flush=false)).
    //
    //   PostVerify_EllMissing           — .ell file not found on disk after
    //                                    successful verify (doInsert step 5).
    //   PostVerify_XmlMissing           — .xml file not found on disk after
    //                                    successful verify (doInsert step 6).
    //   PostVerify_PersistenceIntegrity — the L.9 shared persistence-integrity
    //                                    validator (applyPersistenceIntegrityCheck)
    //                                    returned FaultA_StaleEll or
    //                                    InvalidInput (doInsert step 7).
    PostVerify_EllMissing,
    PostVerify_XmlMissing,
    PostVerify_PersistenceIntegrity,
};

// The SINGLE owner of create-path partial-entry cleanup.
//
// Given that createEntry was attempted and the create-path then failed at
// `stage`, returns whether the partial entry MUST be cleaned up.
//   true  -> createEntry succeeded (progId > 0) and a partial entry exists;
//            the caller MUST remove it (e.g. via deleteEntry with flush=false).
//   false -> createEntry did NOT succeed (progId <= 0); no partial entry
//            exists and no cleanup is required, regardless of stage.
//
// Implemented in src/create_path_cleanup.cpp as the pure function described
// above. The production create-path (doInsert / saveSource / verifyModule)
// routes every failure branch through this single owner.
bool requiresCreatePathCleanup(int progId, CreatePathFailureStage stage);

} // namespace tsdev

#endif // TSDEV_CREATE_PATH_CLEANUP_H
