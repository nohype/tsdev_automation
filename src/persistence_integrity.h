// persistence_integrity.h
//
// Shared persistence-integrity validator API (TVS-L.9, TDD RED).
//
// This header DECLARES the validator used to classify the cross-store
// consistency of a TradeStation EasyLibrary item's two persistence artifacts
// (the compiled `.ell` blob and the `.xml` descriptor) after a modify/upsert
// operation. It exists so that a unit test (tests/test_persistence_integrity.cpp)
// and the production CLI can share ONE definition of what "a successful,
// runtime-linkable refresh" means, instead of each caller re-implementing the
// ELL/XML hash + GUID-occurrence check inline.
//
// STATUS: DECLARATION ONLY. There is NO implementation (.cpp) yet. This is the
// TDD RED step for TVS-L.9: the test in tests/test_persistence_integrity.cpp
// includes this header and calls validatePersistenceIntegrity(), so the test
// executable FAILS TO LINK until a later (GREEN) TVS provides the implementation
// in src/persistence_integrity.cpp. No production code may rely on this symbol
// until the GREEN step ships it.
//
// Why a shared validator is needed (the defect this targets):
//   Fault A (post-compile stale-write false success, see
//   docs/RE/RE_L8_STALE_WRITE_FAULT_A.md): when the `.ell` write fails at the
//   OS level (e.g. read-only file) but ELMDCore.dll's MDWriter::SaveToFile
//   forces `true` and MDVault::SaveModule discards the result, the operation
//   reports `exit 0 / success=true` while the `.ell` is left STALE (hash
//   unchanged) and the `.xml` is rewritten with a NEW GUID that has ZERO
//   occurrences in the stale `.ell`. The two artifacts are inconsistent and the
//   item is runtime-unlinkable, yet the CLI calls it a success. The existing
//   `.ell`/`.xml` existence checks do NOT catch this (both files exist).
//
//   The validator encodes the three observable signals that distinguish a real
//   refresh from Fault A and from a legitimate no-op:
//     1. Did the `.ell` SHA-256 change?            (ellChanged)
//     2. Did the `.xml` SHA-256 change?            (xmlChanged)
//     3. Is the post-op XML's GUID present (exactly once, in its mixed-endian
//        binary form) inside the post-op `.ell`?   (guidInEll)
//   plus the caller-provided precondition:
//     4. Was source-equality proven BEFORE this call? (sourceEqualProven)
//        Only when this is true may an unchanged `.ell` hash be accepted as a
//        no-op rather than rejected as a stale write.
//
// Verdict matrix (what the GREEN implementation must produce):
//
//   ellChanged | xmlChanged | guidInEll | sourceEqualProven | Verdict
//   -----------+------------+-----------+-------------------+----------------------
//      true    |    any     |   true    |       any         | VerifiedRefresh
//      false   |   true     |   false   |      false        | FaultA_StaleEll
//      false   |   false    |   any     |       true        | NoOpAllowed
//      false   |   false    |   any     |      false        | FaultA_StaleEll (*)
//      false   |   true     |   any     |       true        | FaultA_StaleEll (**)
//      true    |    any     |   false   |       any         | FaultA_StaleEll (***)
//   (any unreadable / unparseable input)             | InvalidInput
//
//   (*) Unchanged ELL + unchanged XML but source NOT proven equal: cannot be
//       distinguished from a stale write, so reject (FaultA) rather than
//       silently accept.
//   (**) sourceEqualProven=true but XML changed: the XML GUID changed without
//        the ELL changing, which is the Fault A signature even if the caller
//        believed it was a no-op; reject.
//   (***) ELL changed but the post XML GUID is NOT in the ELL: the ELL was
//        rewritten with something other than the XML's GUID; inconsistent.
//
// The "mixed-endian" GUID form is the standard Windows GUID binary layout
// (Data1 little-endian, Data2 little-endian, Data3 little-endian, Data4
// big-endian / byte-preserved) — the form orlib20/ELMDCore embeds in the `.ell`.
// The validator searches the post-op `.ell` bytes for this 16-byte sequence and
// requires EXACTLY ONE occurrence for a VerifiedRefresh.

#ifndef TSDEV_PERSISTENCE_INTEGRITY_H
#define TSDEV_PERSISTENCE_INTEGRITY_H

#include <string>

namespace tsdev {

// Classification of cross-store (.ell vs .xml) persistence consistency for a
// single modify/upsert operation.
enum class IntegrityVerdict {
    // The `.ell` was rewritten AND the post-op XML's GUID is present (exactly
    // once, mixed-endian) in the new `.ell`. This is a real, runtime-linkable
    // refresh.
    VerifiedRefresh,

    // Source-equality was proven BEFORE the call, and neither artifact changed.
    // An unchanged `.ell` hash is allowed here as a true no-op (the caller
    // already proved the source is identical, so no rewrite was expected).
    // This is NOT a VerifiedRefresh and must not be reported as one.
    NoOpAllowed,

    // Fault A: the `.ell` is stale (hash unchanged, or changed but missing the
    // XML GUID) while the operation would otherwise be expected to have
    // rewritten it. The `.ell` and `.xml` are inconsistent; the item is
    // runtime-unlinkable. The operation must NOT be reported as success.
    FaultA_StaleEll,

    // Input could not be evaluated: a file is missing/unreadable, the XML GUID
    // could not be parsed, a hash could not be computed, etc.
    InvalidInput,
};

// Inputs to the validator. All paths are filesystem paths to the POST-operation
// artifact files. The pre-operation hashes are supplied by the caller (hex
// SHA-256, uppercase or lowercase — the implementation compares case-insensitively)
// because the caller already had the pre-op artifacts in hand. An empty pre-hash
// string means "unknown"; in that case the implementation cannot determine
// ellChanged/xmlChanged and MUST return InvalidInput (it must not guess).
struct IntegrityInput {
    std::string ellPath;       // post-op `.ell` file path
    std::string xmlPath;       // post-op `.xml` file path
    std::string preEllSha256;  // hex SHA-256 of the PRE-op `.ell` (empty = unknown)
    std::string preXmlSha256;  // hex SHA-256 of the PRE-op `.xml` (empty = unknown)
    bool sourceEqualProven;    // true iff the caller proved source-equality before this call
};

// Evaluate the persistence integrity of a single modify/upsert operation.
//
// DECLARATION ONLY — no implementation exists yet (TVS-L.9 RED). Linking a
// caller against this header without a future src/persistence_integrity.cpp
// produces an unresolved external (RED). The GREEN step must implement this
// according to the verdict matrix above and the fixture-driven expectations
// encoded in tests/test_persistence_integrity.cpp.
IntegrityVerdict validatePersistenceIntegrity(const IntegrityInput& in);

} // namespace tsdev

#endif // TSDEV_PERSISTENCE_INTEGRITY_H
