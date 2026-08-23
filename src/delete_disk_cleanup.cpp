// delete_disk_cleanup.cpp
//
// TVS-L.12 GREEN: implementation of the SINGLE decision that classifies
// whether a DeleteFileA failure on a delete-path disk artifact
// (elmodule_<whType>_<progId>.ell / .xml) must FAIL the overall deleteEntry
// Result, instead of today's behavior where every non-ERROR_FILE_NOT_FOUND
// DeleteFileA failure is silently demoted to a stderr WARNING while
// deleteEntry returns true (success).
//
// The decision is a PURE function of (lastError, kind):
//   * ERROR_SUCCESS        (0) -> false  (DeleteFileA succeeded; artifact gone)
//   * ERROR_FILE_NOT_FOUND (2) -> false  (artifact already absent; idempotent
//                                         delete is NOT a failure — the ONE
//                                         case today's code already handles
//                                         correctly)
//   * ERROR_ACCESS_DENIED  (5) -> true   (artifact present but could not be
//                                         removed; the delete Result MUST fail)
//   * any other nonzero lastError -> true (unknown removal failure; fail safe)
//
// The rule is identical for both artifact kinds (Ell and Xml): a stale .ell OR
// a stale .xml left on disk is a real cleanup failure. The `kind` parameter is
// accepted so a future change can differentiate if an artifact is ever proven
// optional, but the contract pinned by tests/test_delete_disk_cleanup.cpp
// requires the same rule for both kinds, so it does not branch the boolean.
//
// This function is deliberately side-effect free: it does not touch orlib20,
// the warehouse, or the filesystem. That keeps it unit-testable with no TS
// dependency (matching the TVS-L.9 / TVS-L.10 pure-unit-test pattern) and makes
// it safe to call from any failure branch without risk of compounding the
// failure.
//
// Per the project's no-hack rule, the Win32 error codes are written as raw
// numeric constants with the intended symbol named in a comment, so this
// translation unit stays self-contained and the pure unit test needs no
// <windows.h> include (the header declares `lastError` as `unsigned long` =
// DWORD on Win32 for the same reason). These are stable Win32 values.

#include "delete_disk_cleanup.h"

namespace tsdev {

bool deleteDiskCleanupErrorFailsResult(unsigned long lastError,
                                       DeleteDiskArtifactKind /*kind*/) {
    // The kind is intentionally unused: the contract pins the SAME rule for
    // both the .ell and the .xml artifact. A stale artifact of either kind
    // left on disk is a real cleanup failure. The parameter is carried so a
    // future change can differentiate if an artifact is ever proven optional,
    // without changing the call sites or the header; the RED test pins the
    // uniform rule, so the boolean must not branch on kind here.

    if (lastError == 0UL) {  // ERROR_SUCCESS — DeleteFileA returned TRUE.
        return false;        // artifact removed; not a failure.
    }
    if (lastError == 2UL) {  // ERROR_FILE_NOT_FOUND — artifact already absent.
        return false;        // idempotent delete; not a failure.
    }
    // ERROR_ACCESS_DENIED (5), ERROR_SHARING_VIOLATION (32), or any other
    // nonzero error: the artifact is present but could not be removed (or the
    // failure is otherwise real/unknown). Fail safe — the delete Result MUST
    // fail so a stale .ell/.xml is never silently treated as success.
    return true;
}

} // namespace tsdev
