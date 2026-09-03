// create_path_cleanup.cpp
//
// TVS-L.10 GREEN: implementation of the SINGLE ownership decision for
// create-path partial-entry cleanup after a successful createEntry.
//
// The decision is a pure function of (progId, stage):
//   * progId <= 0 -> createEntry did NOT succeed; no partial warehouse entry
//                    exists. Returns false for every stage — there is nothing
//                    to clean up, and a caller that blindly deleted would risk
//                    removing an unrelated entry.
//   * progId >  0 -> createEntry DID succeed; a partial warehouse entry now
//                    lives in the in-memory BST. Returns true for EVERY stage
//                    enumerated in CreatePathFailureStage — saveSource failure
//                    AND every verifyModule early-return / SEH / OOM /
//                    compile-error mode. Once createEntry has succeeded, ANY
//                    subsequent failure in the create-path must remove the
//                    partial entry; which step failed only matters for
//                    diagnostics and coverage accounting, not for the boolean.
//
// This function is deliberately side-effect free: it does not touch orlib20,
// the warehouse, or the filesystem. That keeps it unit-testable with no TS
// dependency (matching the TVS-L.9 pure-unit-test pattern) and makes it safe
// to call from any failure branch without risk of compounding the failure.
//
// The stage argument is carried by the caller for two reasons that do NOT
// affect the boolean:
//   1. Diagnostics — the create-path owner can report WHICH post-create step
//      failed (saveSource vs. OOM vs. SEH-ctor vs. ...).
//   2. Coverage — the unit test (tests/test_create_path_cleanup.cpp) iterates
//      every stage so that adding a new failure mode without routing it
//      through this owner is detected.
//
// See src/create_path_cleanup.h for the full contract and the defect this
// targets (cleanup previously scattered / absent across the create-path's
// failure branches).

#include "create_path_cleanup.h"

namespace tsdev {

bool requiresCreatePathCleanup(int progId, CreatePathFailureStage /*stage*/) {
    // The stage is intentionally unused: the contract pins the decision to
    // progId alone. Every enumerator of CreatePathFailureStage represents a
    // failure that occurs AFTER a successful createEntry, so for progId > 0
    // the answer is uniformly true. The stage is carried by the caller for
    // diagnostics and verifiable coverage of every failure path, not for
    // branching here.
    return progId > 0;
}

} // namespace tsdev
