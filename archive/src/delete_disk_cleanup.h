// delete_disk_cleanup.h
//
// Delete-path disk-cleanup failure decision API (TVS-L.12, GREEN).
//
// This header DECLARES the ONE decision point that classifies whether a
// DeleteFileA failure on a delete-path disk artifact (.ell / .xml) must
// cause the overall deleteEntry Result to be a FAILURE, instead of the
// previous behavior where every non-ERROR_FILE_NOT_FOUND DeleteFileA failure
// was silently demoted to a stderr WARNING while deleteEntry still returned
// true (success). It exists so that a unit test
// (tests/test_delete_disk_cleanup.cpp) and the production CLI can share ONE
// definition of "this disk-cleanup failure is real and must fail the delete",
// instead of the success/return-true being unconditional.
//
// STATUS: GREEN. The implementation ships in src/delete_disk_cleanup.cpp and
// is consulted by TsDevLib::deleteEntry step 5 for BOTH the .ell and .xml
// DeleteFileA calls. The unit test in tests/test_delete_disk_cleanup.cpp
// links and passes (the contract below is pinned by its assertions).
// deleteEntry now returns false when EITHER artifact's DeleteFileA failure
// returns true from this decision, instead of returning true unconditionally;
// the residual path + Windows error are surfaced via the optional
// diskCleanupErrorOut out-parameter and the existing stderr WARNING.
//
// Why a single decision is needed (the defect this targets):
//   Before L.12, after the delete orchestrator (FUN_101a8360) and the
//   in-memory BST update (DeleteFromTree) succeeded, deleteEntry step 5
//   deleted the on-disk artifacts elmodule_<whType>_<progId>.ell and .xml
//   via DeleteFileA:
//
//     if (!DeleteFileA(ellPath)) {
//         DWORD err = GetLastError();
//         if (err != ERROR_FILE_NOT_FOUND) {
//             fprintf(stderr, "tsdev: WARNING - failed to delete %s (error=%lu)\n", ...);
//         }
//     }
//     if (!DeleteFileA(xmlPath)) { /* same WARNING-only handling */ }
//     ...
//     return true;   // <-- UNCONDITIONAL success
//
//   So a DeleteFileA failure with, e.g., ERROR_ACCESS_DENIED (5) — the file
//   is present but cannot be removed (read-only / locked / ACL) — was logged
//   to stderr as a WARNING and then deleteEntry returned true. The caller
//   (doDelete, cleanupCreatePathFailure) treated this as a successful delete,
//   even though a stale .ell/.xml was left on disk. That was a disk-cleanup
//   failure treated as success. This API is the single decision that
//   distinguishes a benign idempotent absence (ERROR_FILE_NOT_FOUND) from a
//   real removal failure (ERROR_ACCESS_DENIED and any other nonzero error).
//
// Contract (pinned by the unit test, satisfied by the GREEN implementation):
//   The decision is a PURE function of (lastError, kind). It does not touch
//   orlib20, the warehouse, or the filesystem, so it can be unit-tested with
//   no TS dependency (matching the TVS-L.9 / TVS-L.10 pure-unit-test pattern).
//
//     lastError == ERROR_SUCCESS        (0) -> false  (DeleteFileA succeeded;
//                                                    the artifact is gone)
//     lastError == ERROR_FILE_NOT_FOUND (2) -> false  (artifact already
//                                                    absent; idempotent delete
//                                                    is NOT a failure — this
//                                                    is the ONE case today's
//                                                    code already handles
//                                                    correctly)
//     lastError == ERROR_ACCESS_DENIED  (5) -> true   (artifact present but
//                                                    could not be removed; the
//                                                    delete Result MUST fail)
//     any other nonzero lastError           -> true   (unknown removal
//                                                    failure; fail safe)
//
//   The rule is identical for both artifact kinds (Ell and Xml): a stale
//   .ell OR a stale .xml left on disk is a real cleanup failure. The `kind`
//   parameter makes "both ELL and XML considered" explicit and lets a future
//   change differentiate if an artifact is ever proven optional; the unit
//   test pins the same rule for both kinds.

#ifndef TSDEV_DELETE_DISK_CLEANUP_H
#define TSDEV_DELETE_DISK_CLEANUP_H

namespace tsdev {

// Which delete-path disk artifact a DeleteFileA call targeted. Both are
// written by the create/modify path (elmodule_<whType>_<progId>.<ext>) and
// both are removed by deleteEntry step 5.
enum class DeleteDiskArtifactKind {
    Ell,  // elmodule_<whType>_<progId>.ell  (compiled EasyLanguage module blob)
    Xml,  // elmodule_<whType>_<progId>.xml  (module descriptor / GUID carrier)
};

// The single decision: did this DeleteFileA failure fail the delete Result?
//
// `lastError` is the value returned by GetLastError() immediately after a
// DeleteFileA call on the artifact identified by `kind` (0 / ERROR_SUCCESS
// when DeleteFileA returned TRUE). Returns true when the failure is real and
// the overall deleteEntry Result MUST be a failure; false when the failure is
// benign (success / already-absent) and the delete Result may stay success.
//
// GREEN (TVS-L.12): implemented in src/delete_disk_cleanup.cpp as the pure
// function described above. The production deleteEntry consults it for BOTH
// the .ell and .xml DeleteFileA calls and fails (returns false / a non-success
// Result) when EITHER artifact's error returns true, instead of returning
// true unconditionally; the residual path + Windows error are surfaced via
// the optional diskCleanupErrorOut out-parameter and the existing stderr
// WARNING. Note: this decision does NOT itself remove the read-only attribute
// or otherwise recover the artifact — it only classifies the failure so the
// caller can report it instead of silently treating it as success. Physical
// sanitation of a residual read-only ELL remains the responsibility of the
// harness finally block (or the operator), not this function.
//
// Note: `lastError` is declared as `unsigned long` (DWORD = unsigned long on
// Win32) so this header stays self-contained and the pure unit test needs no
// <windows.h> include. The implementation / production caller, which already
// has <windows.h>, may pass a DWORD directly.
bool deleteDiskCleanupErrorFailsResult(unsigned long lastError,
                                       DeleteDiskArtifactKind kind);

} // namespace tsdev

#endif // TSDEV_DELETE_DISK_CLEANUP_H
