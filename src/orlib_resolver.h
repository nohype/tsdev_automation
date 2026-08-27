// =============================================================================
// orlib_resolver.h
//
// TVS-B.3 — Design of the OrlibResolver abstraction layer.
//
// Encapsulates ALL version-specific knowledge of orlib20.dll behind a single
// class so that the rest of the CLI (TsDevLib) never touches a hardcoded RVA,
// a raw HMODULE, or a byte signature. The resolver:
//   (a) loads orlib20.dll and captures the base address,
//   (b) detects the DLL version (3-tier method, TVS-B.2),
//   (c) resolves every internal function by scanning the signature set
//       selected for the detected version (catalog TVS-B.1),
//   (d) validates patch sites (expected original bytes) before patches,
//   (e) hands typed function pointers to the caller.
//
// This header originated as the TVS-B.3 design interface. The implementation
// is src/orlib_resolver.cpp (TVS-B.4), and TsDevLib uses it through the
// resolver integration. Method bodies remain declared-only (no inline
// definitions); the header compiles standalone and consumers link the
// implementation library.
//
// Target: 32-bit x86 (Win32). orlib20.dll is a 32-bit PE. All function-pointer
// typedefs use __stdcall for exported (_fn@N decorated) functions and match the
// calling conventions confirmed in docs/SIGNATURE_CATALOG.md for internal
// (RVA-resolved) functions.
//
// -----------------------------------------------------------------------------
// DESIGN RATIONALE
// -----------------------------------------------------------------------------
//
// 1. WHY SIGNATURE SCANNING OVER HARDCODED RVAs
//
//   The production code (src/tsdev_lib.cpp) resolves all internal-function
//   and patch-site RVAs at runtime via OrlibResolver (signature-based
//   resolution) rather than as hardcoded compile-time constants. This avoids
//   the fragility of fixed RVAs: a hardcoded RVA is valid only for a single
//   tested build (FileVersion 9.50.01.3344; see
//   signatures/v9.50.01.3344.json for the PE timestamp and all version
//   indicators). If TradeStation ships a rebuild — even one with the same
//   FileVersion — the linker may shift code, and every hardcoded RVA would
//   silently point at the wrong bytes. The result is a crash or, worse, a
//   call into unrelated code that corrupts state.
//
//   Signature scanning locates each function by its byte pattern (prologue +
//   distinctive instruction sequence) rather than by a fixed offset. As long
//   as the function's prologue and structural instructions are preserved
//   across a rebuild, the signature resolves to the correct address
//   automatically. Wildcards (??) cover only address-dependent bytes (SEH
//   handler addresses, data-section references, IAT slots, relative
//   call/jump displacements) so the signature is stable under relocation and
//   minor code shifts. See docs/SIGNATURE_CATALOG.md §2 for the wildcard
//   policy.
//
//   Trade-off: signature scanning is slower than RVA arithmetic (O(text_size *
//   sig_len) per target) and depends on the signature remaining unique. Both
//   are acceptable: the .text section is ~2 MB and there are signature
//   definitions (tracked call targets and patch sites; see
//   docs/SIGNATURE_CATALOG.md for the current counts), so the full scan
//   completes in milliseconds at startup.
//
// 2. HOW THE VERSION MANIFEST WORKS
//
//   The manifest (OrlibVersionManifestEntry, from docs/VERSION_DETECTION.md
//   §6.2) is a compiled-in constexpr array — one entry per known-tested DLL
//   version. Each entry carries tiered identity indicators:
//     Tier 1: FileVersion string (primary key, cheap, human-readable).
//     Tier 2: PE timestamp + file size (catches rebuilds sharing a FileVersion).
//     Tier 3: SHA-256 (definitive byte-identity, expensive — computed last).
//   plus .text/export sanity indicators and a SignatureSetId that selects
//   which signature set to use for resolution.
//
//   detectVersion() implements the 3-tier decision tree
//   (docs/VERSION_DETECTION.md §4.2): Tier 1 lookup → Tier 2 cross-check →
//   Tier 3 hash confirmation. The result is a structured VersionDescriptor
//   (known/unknown, tier-match level, matched entry pointer). Version
//   detection NEVER aborts the CLI on its own — it only emits warnings.
//   The hard failure comes from signature resolution: if the signatures do
//   not match, the resolver throws. This separates "untested version"
//   (warning) from "signatures don't match" (hard error).
//
// 3. MANIFEST ROLE IN SIGNATURE RESOLUTION
//
//   Runtime address resolution is signature-only. The resolver scans the
//   selected .text signature set and requires exactly one match for each
//   target; zero or multiple matches throw OrlibResolverError. Manifest data
//   does not provide runtime RVA addresses or an alternative after a failed
//   signature scan.
//
//   The manifest's runtime role is version DETECTION (is this a known build?)
//   and SIGNATURE-SET SELECTION (which catalog applies to that version). For
//   patch sites, the signature match supplies the containing-function address
//   and patchOffset computes the patch address; the signature catalog supplies
//   the expected original and replacement bytes used for validation/application.
//
//   If the version is unknown (not in the manifest), signature scanning still
//   runs and may succeed when the code preserves the required patterns. A
//   missing manifest entry therefore produces a version warning, not an RVA
//   substitute or an automatic resolution failure; a zero- or multi-match
//   scan remains a hard resolution error.
//
//   The expectedRva value retained in each signature definition is diagnostic
//   metadata for the tested build only. It is not used to resolve a target at
//   runtime.
//
// 4. ERROR-HANDLING STRATEGY
//
//   The resolver uses exceptions, not return codes, for all UNRECOVERABLE
//   resolution failures. A custom exception type (OrlibResolverError, derived
//   from std::runtime_error) carries structured context (target name, match
//   count, hex dump) so the caller can produce a clear diagnostic.
//
//   - Signature no-match (scan count = 0): throw OrlibResolverError with the
//     signature name, the hex pattern, and a hex dump of the searched .text
//     region (first/last 256 bytes) so the failure is diagnosable.
//   - Signature multi-match (scan count > 1): throw OrlibResolverError with
//     the signature name, match count, and all match RVAs. A multi-match
//     means the signature is not unique and must be extended (as was done
//     for Target 12 during TVS-B.1).
//   - Export resolution failure (GetProcAddress returns NULL): throw
//     OrlibResolverError with the decorated export name and GetLastError().
//   - Patch-site validation failure (bytes don't match expected): throw
//     OrlibResolverError with the patch-site name, RVA, expected vs. actual
//     bytes.
//   - Version detection: NEVER throws. Unknown / rebuilt / byte-differs
//     states are warnings (stderr), not exceptions. See §2 above.
//
//   Recoverable operations (e.g., "is this patch already applied?") use bool
//   return values, not exceptions.
//
// 5. UNIT-TESTABILITY WITHOUT A LIVE TS INSTALL
//
//   The resolver exposes a test seam: a protected virtual method
//   (scanTextSection) that performs the signature scan over a byte buffer.
//   The production path feeds the loaded DLL's .text section; the test path
//   feeds an in-memory buffer (e.g., a copy of orlib20_baseline.dll read from
//   disk, or a synthetic buffer containing just the signature bytes).
//
//   Two constructor overloads provide the seam:
//     - OrlibResolver(const wchar_t* dllPath): loads the real DLL via
//       LoadLibraryW (production).
//     - OrlibResolver(const uint8_t* image, size_t size, uintptr_t base):
//       operates on an in-memory PE image WITHOUT loading it (tests). No
//       HMODULE is acquired; VirtualProtect is a no-op in this mode; export
//       resolution is simulated by parsing the PE export table from the
//       buffer. This lets test_orlib_resolver run on any machine without a
//       TradeStation install.
//
//   Subclasses can override scanTextSection() to inject custom scan behavior
//   (e.g., fuzzing the signature matcher).
//
// 6. INTEGRATION WITH TsDevLib
//
//   The resolver is an INTERNAL refactor. TsDevLib's public API
//   (findExistPos, createEntry, saveSource, verifyModule, closeWarehouse,
//   deleteEntry, modifyEntry, ellFileExists, getEllPath, getXmlPath)
//   is UNCHANGED. The integration
//   (TVS-B.5) replaces the constructor's hardcoded RVA arithmetic and
//   GetProcAddress calls with OrlibResolver calls:
//
//     Before (tsdev_lib.cpp):
//       m_orlib = LoadLibraryW(L"orlib20.dll");
//       m_orlibBase = (DWORD_PTR)m_orlib;
//       m_pStartCompiling = (StartCompilingProcess_t)(m_orlibBase + <RVA>);
//       m_pOrlibInit = resolveOrThrow<OrlibInit_t>("_OrlibInit@8");
//       applyThreadCheckPatches();  // uses hardcoded RVAs
//
//     After (TVS-B.5):
//       OrlibResolver resolver(L"orlib20.dll");
//       m_pStartCompiling = resolver.resolveStartCompilingProcess();
//       m_pOrlibInit      = resolver.resolveOrlibInit();
//       resolver.applyPatch("FUN_101d0950_thread_check_INT3");
//
//   The resolver owns the HMODULE and base address; TsDevLib holds a resolver
//   instance (or pointer) and queries it. No public method signature on
//   TsDevLib changes.
//
// 7. HISTORICAL DISCREPANCY NOTE: "8 exports"
//
//   The TVS-B.3 handoff's adversarial-review line mentioned "8 exports". At
//   that time, docs/SIGNATURE_CATALOG.md §5 listed 7 export names used in
//   production code. F.4/F.5 later added 3 named exports, and M.1/M.3 added
//   ordinal-only exports 66, 67, and 57. The current tracked export set
//   (by-name + ordinal-only) is recorded in docs/SIGNATURE_CATALOG.md,
//   separate from the total PE export count. This header follows the current
//   catalog.
//
// =============================================================================

#ifndef ORLIB_RESOLVER_H
#define ORLIB_RESOLVER_H
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <stdexcept>
#include <windows.h>

namespace tsdev {

// ---------------------------------------------------------------------------
// Forward reference to the TVS-B.1 signature definitions.
// signatures/v9.50.01.3344.json (source of truth) is code-generated into
// signatures.gen.h, which defines SignatureDef and kSignatures[] (see
// docs/SIGNATURE_CATALOG.md for the current signature-definition count).
// The resolver consumes these at runtime. The struct is re-declared here
// (as OrlibSignatureDef) so this header is self-contained and does not
// hard-include a generated artifact; the implementation (TVS-B.4) bridges
// the two.
// ---------------------------------------------------------------------------

/// \brief A single byte signature with wildcard mask.
///
/// Mirrors `SignatureDef` from `signatures.gen.h` (generated from
/// `signatures/v9.50.01.3344.json`) but uses
/// structured byte arrays instead of hex strings so the resolver can scan
/// without parsing at runtime. The implementation (TVS-B.4) converts the
/// `kSignatures[]` hex patterns into this form at startup (or compile time).
struct OrlibSignatureDef {
    const char*        name;                 ///< Human-readable target name (matches SIGNATURE_CATALOG.md).
    int                expectedRva;          ///< Expected RVA for the tested build (diagnostic only; not used for resolution).
    const uint8_t*     bytes;                ///< Fixed-byte values (length = `length`).
    const uint8_t*     mask;                 ///< Per-byte mask: 0xFF = fixed (must match), 0x00 = wildcard (??, matches any).
    int                length;               ///< Number of bytes in `bytes` / `mask`.
    int                expectedMatchCount;   ///< Expected unique matches in .text (always 1).
    bool               isPatchSite;          ///< True for current patch-site targets (count: see docs/SIGNATURE_CATALOG.md).
    const uint8_t*     originalBytes;        ///< Patch sites: original bytes at the patch site.
    int                originalBytesLen;     ///< Patch sites: length of `originalBytes`.
    const uint8_t*     originalBytesMask;    ///< Patch sites: per-byte mask for original bytes (0xFF=check, 0x00=wildcard).
    int                originalBytesMaskLen; ///< Patch sites: length of `originalBytesMask`.
    const uint8_t*     replacementBytes;     ///< Patch sites: replacement bytes to write.
    int                replacementBytesLen;  ///< Patch sites: length of `replacementBytes`.
    int                patchOffset;          ///< Patch sites: byte offset from signature match point to patch site.
    const char*        containingFunction;   ///< Patch sites: name of the containing function (diagnostic).
};

// ---------------------------------------------------------------------------
// Version manifest (from docs/VERSION_DETECTION.md §6.2)
// ---------------------------------------------------------------------------

/// \brief Identifies which signature set to use for a DLL version.
/// \see docs/VERSION_DETECTION.md §6.2.
///
/// The full enum definition (with enumerators SIGSET_NONE, SIGSET_V9_50_01_3344,
/// etc.) is OWNED by the generated header signatures.gen.h, which defines it
/// BEFORE including orlib_resolver.h. This forward declaration is sufficient
/// for using SignatureSetId as a by-value struct member (the underlying type
/// uint16_t is fixed, so the size is known). Translation units that reference
/// enum VALUES (e.g. SIGSET_V9_50_01_3344) must include signatures.gen.h to
/// get the full definition.
enum SignatureSetId : uint16_t;

/// \brief One entry in the known-versions manifest.
///
/// Maps orlib20.dll version indicators to a signature set. Compiled in as a
/// constexpr array (no runtime JSON parsing). One entry per known-tested DLL
/// version. Currently one entry (9.50.01.3344).
///
/// \see docs/VERSION_DETECTION.md §6.2 for field-by-field documentation.
struct OrlibVersionManifestEntry {
    // --- Tier 1: primary key (PE version resource) ---
    const char* fileVersion;       ///< e.g. "9.50.01.3344" (manifest key).

    // --- Tier 2: secondary cross-check (cheap, catches rebuilds) ---
    uint32_t    peTimestamp;       ///< COFF TimeDateStamp (see signatures/v9.50.01.3344.json).
    uint32_t    fileSize;          ///< File size in bytes (e.g. 4924400).

    // --- Tier 3: tertiary confirmation (definitive, expensive) ---
    const char* sha256;            ///< Uppercase hex, 64 chars, no spaces.

    // --- Additional indicators (sanity checks, near-free) ---
    uint32_t    textVsize;         ///< .text virtual size (see signatures/v9.50.01.3344.json).
    uint32_t    textRawSize;       ///< .text raw size (see signatures/v9.50.01.3344.json).
    uint32_t    textRva;           ///< .text RVA (see signatures/v9.50.01.3344.json).
    uint32_t    imageBase;         ///< Optional header ImageBase (see signatures/v9.50.01.3344.json).
    uint16_t    exportCount;       ///< NumberOfFunctions (e.g. 699).
    uint16_t    exportNamedCount;  ///< NumberOfNames (e.g. 636).
    uint16_t    exportHighestOrdinal; ///< e.g. 708.
    uint16_t    exportOrdinalBase;    ///< e.g. 10.

    // --- PE version resource metadata (sanity / human-readable) ---
    const char* productVersion;    ///< e.g. "9.50.01.3344".
    const char* productName;       ///< e.g. "EasyLanguage".
    const char* fileDescription;   ///< e.g. "Omega EasyLanguage".
    const char* companyName;       ///< e.g. "TradeStation Technologies, Inc."
    const char* internalName;      ///< e.g. "OR_LIB".
    const char* originalFilename;  ///< e.g. "OR_LIB.DLL".

    // --- Signature set reference ---
    SignatureSetId signatureSet;   ///< Which signature set to use for this version.
};

// ---------------------------------------------------------------------------
// Version descriptor (return type of detectVersion())
// ---------------------------------------------------------------------------

/// \brief Tier-match level reached during version detection.
/// \see docs/VERSION_DETECTION.md §4.1.
enum class VersionTier : uint8_t {
    Unknown         = 0,  ///< FileVersion not in manifest.
    Tier1Match      = 1,  ///< FileVersion found; Tier 2 not yet checked.
    Tier2Match      = 2,  ///< PE timestamp + file size match (known, same build).
    Tier2Mismatch   = 3,  ///< FileVersion found but timestamp/size differ (known but rebuilt).
    Tier3Match      = 4,  ///< SHA-256 matches (known & verified, byte-identical).
    Tier3Mismatch   = 5,  ///< SHA-256 differs (known but byte-differs).
};

/// \brief Structured result of version detection.
///
/// Returned by OrlibResolver::detectVersion(). Never represents a fatal
/// error — unknown/rebuilt/byte-differs are warnings, not exceptions.
struct VersionDescriptor {
    bool                          known;            ///< True if FileVersion was found in the manifest.
    VersionTier                   tier;             ///< Highest tier reached.
    std::string                   fileVersion;      ///< FileVersion string read from the DLL.
    uint32_t                      peTimestamp;      ///< PE timestamp read from the DLL.
    uint32_t                      fileSize;         ///< File size read from the DLL.
    std::string                   sha256;           ///< SHA-256 of the on-disk file (empty if not computed).
    const OrlibVersionManifestEntry* manifestEntry; ///< Matched manifest entry, or nullptr if unknown.
    SignatureSetId                signatureSet;     ///< Selected signature set (SIGSET_NONE if unknown).
    std::string                   warningMessage;   ///< Human-readable warning (empty if known & verified).
};

// ---------------------------------------------------------------------------
// Patch-site descriptor (return type of resolvePatchSite)
// ---------------------------------------------------------------------------

/// \brief Describes a resolved patch site (address + expected/original bytes).
struct PatchSiteDescriptor {
    std::string         name;             ///< Patch-site name (e.g. "FUN_101d0950_thread_check_INT3").
    uintptr_t           patchRva;         ///< RVA of the patch site (where bytes are written).
    uintptr_t           sigMatchRva;      ///< RVA where the signature matched (containing function entry).
    int                 patchOffset;      ///< Offset from sig match to patch site.
    std::vector<uint8_t> originalBytes;   ///< Expected original bytes at the patch site (for validation).
    std::vector<uint8_t> replacementBytes;///< Replacement bytes to write.
    std::string         containingFunction; ///< Name of the containing function (diagnostic).
};

// ---------------------------------------------------------------------------
// Exception type
// ---------------------------------------------------------------------------

/// \brief Exception thrown by OrlibResolver for unrecoverable resolution
///        failures.
///
/// Derived from std::runtime_error so existing catch handlers in TsDevLib
/// (which catch std::runtime_error) continue to work. Carries structured
/// diagnostic context in the `detail` string: target name, match count,
/// expected vs. actual bytes, hex dumps, etc.
class OrlibResolverError : public std::runtime_error {
public:
    /// \param whatArg Short description (e.g. "signature no-match").
    /// \param target  Target name (e.g. "StartCompilingProcess") or export name.
    /// \param detail  Full diagnostic (hex dump, match RVAs, etc.).
    OrlibResolverError(const std::string& whatArg,
                       const std::string& target,
                       const std::string& detail)
        : std::runtime_error(whatArg + ": target='" + target + "' " + detail)
        , m_target(target)
        , m_detail(detail)
    {}

    const std::string& target() const  { return m_target; }
    const std::string& detail() const  { return m_detail; }

private:
    std::string m_target;
    std::string m_detail;
};

// ===========================================================================
// Typed function-pointer typedefs
//
// These match the typedefs already in tsdev_lib.h (and the SEH-wrapper
// typedefs in tsdev_lib.cpp) so the resolver's return values can be assigned
// directly to TsDevLib's member variables without adapter casts.
//
// Exports: all __stdcall (stdcall-decorated _fn@N, except FindExistPos which
//   is exported undecorated but still uses __stdcall calling convention).
// Internal functions: calling conventions per docs/SIGNATURE_CATALOG.md §3.
// ===========================================================================

// --- 7 core named exports (all __stdcall) ---
typedef BOOL (__stdcall *OrlibInitFn)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExitFn)(void);
typedef int  (__stdcall *FindExistPosFn)(const char* name, int warehouseType);
typedef int  (__stdcall *CreateEntryEXFn)(int whType, int secondary,
                                          const char* name, int param4,
                                          char param5, int* errCode);
typedef int  (__stdcall *SaveModule_NewFn)(int warehouseType, int progId,
                                           void* arg3, HWND editorHwnd,
                                           void* arg5);
typedef int  (__stdcall *VerifyModule_NewFn)(HWND hwnd, int warehouseType,
                                             int progId, int flag,
                                             void* compilerObj);
typedef void (__stdcall *CloseWarehouseFn)(int param);

// --- Typed internal functions (CALL_TARGETs, resolved by signature scan) ---
// Target 1: StartCompilingProcess — __cdecl, no args.
typedef void (__cdecl *StartCompilingProcessFn)(void);
// Target 2: EndCompilingProcess — __fastcall, 1 arg.
typedef void (__fastcall *EndCompilingProcessFn)(int param);
// Target 5: InitGlobalLookup — __cdecl, no args, returns global lookup ptr.
typedef void* (__cdecl *InitGlobalLookupFn)(void);
// Target 6: DeleteFromTree — __thiscall (ecx=globalLookup; stack: whType,
//   progId, flag; RET 12). MSVC has no __thiscall function-pointer type, so
//   stored as void* and invoked via inline asm (see callDeleteFromTree in
//   tsdev_lib.cpp). The resolver returns void* for this target.
// Target 7: GetWarehouse — __cdecl, 1 arg (path; 0=default).
typedef void* (__cdecl *GetWarehouseFn)(int pathParam);
// Target 8: FUN_101a8360 delete orchestrator — __fastcall (ECX=whType,
//   EDX=progId, no stack args). Invoked via inline asm in production code.
//   Stored as void*.

// --- Workstream M slice M.1: CELCompiler alloc/destroy via orlib20 ordinals ---
// TSDev allocates+constructs the CELCompiler via orlib20 ordinal export 66
// (FUN_10165f70 = operator_new(0x58) + ctor FUN_10165a00) and destructs+frees
// via ordinal 67 (FUN_10165fd0 = dtor FUN_10165ac0 + operator delete(p,0x58)).
// Calling conventions are documented in the historical M.1 handoff
// (`docs/M1_HANDOFF.md` §R-M.1.1); the raw Ghidra note is transient and
// unavailable in a fresh checkout:
//   Ordinal 66: __cdecl, no args, returns void* (the constructed object, or
//               NULL on alloc failure). Ends with plain RET (C3).
//   Ordinal 67: __stdcall, one arg (void* p), returns void. Ends with
//               RET 4 (C2 04 00). Getting this wrong corrupts the stack.
// These REPLACE the old signature-scanned CELCompilerCtorFn / CELCompilerDtorFn
// (raw ctor/dtor at RVA 0x165A00 / 0x165AC0), which TSDev never calls
// directly — TSDev reaches them exclusively through the ordinal-66/67
// wrappers. The raw ctor/dtor signatures have been removed from the
// signature catalog (R-M.1.1).
typedef void* (__cdecl *CELCompilerAllocFn)(void);      // ordinal 66
typedef void  (__stdcall *CELCompilerDestroyFn)(void*); // ordinal 67

// Workstream M slice M.3: 0x40A library-change notification sender.
// orlib20 ordinal 57 (FUN_101526f0), __stdcall, RET 8 (2 dword args):
//   void NotifyLibChanged(unsigned short category, unsigned short progId)
// Internally packs lParam=(category<<16)|(progId&0xFFFF), wParam=0,
// msg=0x40A, and calls tslib!PosMessageToClients (async PostMessageW to
// the registered client HWND set). Always returns 1 (the return value is
// NOT a delivery guarantee — see docs/RE/RE_M3_0x40A_NOTIFICATION.md §3.1).
// The wrapper is an ordinal-only [NONAME] export, so it is resolved via
// resolveExportByOrdinal(57), not by name.
typedef void (__stdcall *NotifyLibChangedFn)(unsigned short category,
                                             unsigned short progId); // ordinal 57

// ===========================================================================
// OrlibResolver
// ===========================================================================

/// \brief Encapsulates all version-specific knowledge of orlib20.dll.
///
/// Loads the DLL, uses manifest data for version detection and signature-set
/// selection, resolves internal functions by signature scan, validates patch
/// sites, and provides typed function pointers to the caller.
///
/// \par Construction
/// Two constructor overloads:
/// - **Production**: `OrlibResolver(const wchar_t* dllPath)` — loads the real
///   DLL via LoadLibraryW, acquires the HMODULE, parses PE headers.
/// - **Test**: `OrlibResolver(const uint8_t* image, size_t size,
///   uintptr_t base)` — operates on an in-memory PE image buffer WITHOUT
///   loading the DLL. No HMODULE is acquired; VirtualProtect is a no-op;
///   exports are resolved by parsing the PE export table from the buffer.
///   This allows unit tests to run without a live TradeStation install.
///
/// \par Lifecycle
/// The resolver is constructed early in TsDevLib's constructor (TVS-B.5),
/// before any orlib20 function is called. It is NOT copyable (owns the
/// HMODULE / image buffer); it IS movable (transfers ownership).
///
/// \par Error handling
/// Unrecoverable failures throw OrlibResolverError (derived from
/// std::runtime_error). Version detection never throws (warnings only).
/// See the top-of-file design rationale for the full strategy.
class OrlibResolver {
public:
    // --- Construction / destruction ----------------------------------------

    /// \brief Production constructor: load the real orlib20.dll.
    /// \param dllPath Path to orlib20.dll (or just L"orlib20.dll" if the
    ///                DLL directory is on the search path).
    /// \throws OrlibResolverError if LoadLibraryW fails or the PE headers
    ///         are invalid (not a PE32 / x86 image).
    explicit OrlibResolver(const wchar_t* dllPath);

    /// \brief Test constructor: operate on an in-memory PE image buffer.
    ///
    /// Does NOT call LoadLibraryW. The buffer must contain a complete PE32
    /// image (e.g., the contents of orlib20_baseline.dll read from disk).
    /// Export resolution parses the PE export table from the buffer;
    /// VirtualProtect / patch application are no-ops (the buffer is
    /// read-only). Signature scanning operates on the .text section parsed
    /// from the buffer. This is the unit-test seam.
    ///
    /// \param image  Pointer to the in-memory PE image buffer.
    /// \param size   Size of the buffer in bytes.
    /// \param base   Simulated image base address (for RVA arithmetic in
    ///               tests). Use the real DLL's ImageBase (see
    ///               signatures/v9.50.01.3344.json) to match, or any
    ///               value — the resolver uses RVAs internally.
    /// \throws OrlibResolverError if the buffer is too small or the PE
    ///         headers are invalid.
    OrlibResolver(const uint8_t* image, size_t size, uintptr_t base);

    /// \brief Destructor: does not release the HMODULE (not released in production mode).
    ///
    /// In production mode, FreeLibrary is NOT called (matching TsDevLib's
    /// existing behavior — internal threads/state may still reference the
    /// DLL). In test mode, nothing to release (buffer is caller-owned).
    ~OrlibResolver();

    OrlibResolver(const OrlibResolver&)            = delete;
    OrlibResolver& operator=(const OrlibResolver&) = delete;
    OrlibResolver(OrlibResolver&&)                 = default;
    OrlibResolver& operator=(OrlibResolver&&)      = default;

    // --- Version detection -------------------------------------------------

    /// \brief Detect the orlib20.dll version using the 3-tier method.
    ///
    /// Implements the decision tree from docs/VERSION_DETECTION.md §4.2:
    ///   1. Read FileVersion from the PE version resource.
    ///   2. Look up FileVersion in kOrlibVersionManifest (Tier 1).
    ///   3. Compare PE timestamp + file size (Tier 2).
    ///   4. Compute and compare SHA-256 of the on-disk file (Tier 3).
    ///
    /// NEVER throws. Unknown / rebuilt / byte-differs states are returned
    /// as warnings in the VersionDescriptor, not as exceptions. The caller
    /// (TsDevLib constructor) prints the warning to stderr and proceeds.
    ///
    /// \return VersionDescriptor with known flag, tier level, matched
    ///         manifest entry (or nullptr), selected SignatureSetId, and
    ///         warning message.
    VersionDescriptor detectVersion();

    /// \brief Get the result of the most recent detectVersion() call.
    ///
    /// Returns a copy of the cached VersionDescriptor. detectVersion() is
    /// called once at construction; this accessor lets the caller query the
    /// result later without re-running detection.
    const VersionDescriptor& version() const;

    // --- Generic signature resolution --------------------------------------

    /// \brief Resolve a target by scanning its signature in the .text section.
    ///
    /// This is the generic resolution method. Every specific resolve* method
    /// (resolveStartCompilingProcess, etc.) calls this internally with the
    /// appropriate signature from the catalog. Callers may also use this
    /// directly to resolve ad-hoc signatures (e.g., for experimental targets).
    ///
    /// \par Signature-scan algorithm
    /// 1. Parse the hex pattern into a byte array + wildcard mask (`??` →
    ///    mask byte 0x00, `XX` → mask byte 0xFF).
    /// 2. Scan the .text section raw bytes: for each offset `i` in
    ///    `[0, textRawSize - sigLen]`, check if all non-wildcard bytes match.
    /// 3. Collect all match offsets; convert to RVAs (`textRva + offset`).
    /// 4. **Uniqueness enforcement**: exactly 1 match is required.
    ///       - 0 matches → throw OrlibResolverError("signature no-match")
    ///         with the signature name, hex pattern, and a hex dump of the
    ///         first and last 256 bytes of .text (so the failure is
    ///         diagnosable without a debugger).
    ///       - >1 matches → throw OrlibResolverError("signature multi-match")
    ///         with the signature name, match count, and all match RVAs.
    ///         The signature must be extended to restore uniqueness (as was
    ///         done for Target 12 during TVS-B.1).
    /// 5. Return the match RVA.
    ///
    /// \param name      Target name (for diagnostics; e.g. "StartCompilingProcess").
    /// \param signature The signature definition (bytes, mask, length).
    /// \return The RVA where the signature uniquely matched.
    /// \throws OrlibResolverError on no-match or multi-match.
    uintptr_t resolveBySignature(const std::string& name,
                                 const OrlibSignatureDef& signature);

    /// \brief Resolve a target by signature, returning an absolute address.
    ///
    /// Convenience wrapper: `baseAddress() + resolveBySignature(name, sig)`.
    /// \throws OrlibResolverError on no-match or multi-match.
    void* resolveAddressBySignature(const std::string& name,
                                    const OrlibSignatureDef& signature);

    // --- Signature catalog access ------------------------------------------

    /// Get the number of loaded signature definitions in the current signature set.
    /// @return Number of signatures (see docs/SIGNATURE_CATALOG.md for the
    ///         current baseline-version count).
    size_t getSignatureCount() const;

    /// Get a signature definition by index (for enumeration/iteration).
    /// @param index Zero-based index into the loaded signature set.
    /// @return Const reference to the signature definition at @p index.
    /// @throws std::out_of_range if @p index >= getSignatureCount().
    const OrlibSignatureDef& getSignatureByIndex(size_t index) const;

    // --- Specific internal-function resolvers -------------------------------
    //
    // Each signature-resolved method resolves its target by signature scan
    // (via resolveBySignature) and returns a typed function pointer cast to
    // the appropriate calling convention. The current catalog has
    // call-target signature definitions (count: see
    // docs/SIGNATURE_CATALOG.md); ordinal-backed M.1/M.3 helpers are
    // documented with their methods below.
    //
    // On signature no-match or multi-match, each throws OrlibResolverError.
    // ------------------------------------------------------------------------

    /// \brief Resolve Target 1: StartCompilingProcess (__cdecl).
    /// RVA resolved at runtime via signature scan (see
    /// signatures/v9.50.01.3344.json).
    /// \return Typed function pointer (StartCompilingProcessFn).
    /// \throws OrlibResolverError if the signature does not match uniquely.
    StartCompilingProcessFn resolveStartCompilingProcess();

    /// \brief Resolve Target 2: EndCompilingProcess (__fastcall).
    /// RVA resolved at runtime via signature scan (see
    /// signatures/v9.50.01.3344.json).
    /// \return Typed function pointer (EndCompilingProcessFn).
    /// \throws OrlibResolverError if the signature does not match uniquely.
    EndCompilingProcessFn resolveEndCompilingProcess();

    /// \brief Resolve Target 3 (M.1): CELCompiler alloc+ctor via orlib20
    /// ordinal export 66 (__cdecl, no args, returns void*).
    ///
    /// Workstream M slice M.1 replaced the old signature-scanned
    /// resolveCELCompilerCtor() (raw ctor at RVA 0x165A00) with ordinal-based
    /// resolution. TSDev reaches the CELCompiler constructor EXCLUSIVELY
    /// through orlib20 ordinal export 66 (FUN_10165f70 = operator_new(0x58)
    /// + ctor FUN_10165a00), NEVER via a direct call to the raw ctor. The
    /// ordinal wrapper allocates the 0x58-byte object AND constructs it in
    /// one call, returning the pointer (or NULL on alloc failure). The
    /// caller must NOT separately malloc/memset the object — ordinal 66 does
    /// both internally.
    ///
    /// Calling convention (historical M.1 handoff, see
    /// `docs/M1_HANDOFF.md` §R-M.1.1; raw Ghidra note is transient/unavailable):
    /// __cdecl, no args, returns void*. Ends
    /// with plain RET (C3).
    ///
    /// \return Typed function pointer (CELCompilerAllocFn).
    /// \throws OrlibResolverError if ordinal 66 is not in the export table.
    CELCompilerAllocFn resolveCELCompilerAlloc();

    /// \brief Resolve Target 4 (M.1): CELCompiler dtor+free via orlib20
    /// ordinal export 67 (__stdcall, one arg void* p, returns void).
    ///
    /// Workstream M slice M.1 replaced the old signature-scanned
    /// resolveCELCompilerDtor() (raw dtor at RVA 0x165AC0) with ordinal-based
    /// resolution. TSDev reaches the CELCompiler destructor EXCLUSIVELY
    /// through orlib20 ordinal export 67 (FUN_10165fd0 = dtor FUN_10165ac0
    /// + operator delete(p, 0x58)). The ordinal wrapper destructs AND frees
    /// the object in one call. The caller must NOT separately std::free the
    /// pointer — ordinal 67 does both internally (calling std::free after
    /// would double-free).
    ///
    /// Calling convention (historical M.1 handoff, see
    /// `docs/M1_HANDOFF.md` §R-M.1.1; raw Ghidra note is transient/unavailable):
    /// __stdcall, one arg (void* p), returns void. Ends with RET 4 (C2 04 00).
    /// Getting this wrong corrupts the
    /// stack.
    ///
    /// \return Typed function pointer (CELCompilerDestroyFn).
    /// \throws OrlibResolverError if ordinal 67 is not in the export table.
    CELCompilerDestroyFn resolveCELCompilerDestroy();

    /// \brief Resolve Target 4b (M.3): 0x40A library-change notification
    /// sender via orlib20 ordinal export 57 (__stdcall, 2 dword args,
    /// returns void).
    ///
    /// Workstream M slice M.3: TSDev posts the 0x40A library-change
    /// notification by calling orlib20 ordinal 57 (FUN_101526f0) after a
    /// successful verify (FUN_00453f70) and after a successful
    /// category-gated delete (FUN_10159ae0). The wrapper is an ordinal-only
    /// [NONAME] export, so it is resolved via resolveExportByOrdinal(57),
    /// not by name. See docs/RE/RE_M3_0x40A_NOTIFICATION.md §1.2 (R-M.3.1).
    ///
    /// Calling convention (CONFIRMED by Ghidra disassembly):
    /// __stdcall, RET 8 (C2 08 00) — 2 dword args:
    ///   void NotifyLibChanged(unsigned short category,
    ///                         unsigned short progId)
    /// Internally packs lParam=(category<<16)|(progId&0xFFFF), wParam=0,
    /// msg=0x40A, and calls tslib!PosMessageToClients (async PostMessageW
    /// to the registered client HWND set). Always returns 1 (the return
    /// value is NOT a delivery guarantee — see
    /// docs/RE/RE_M3_0x40A_NOTIFICATION.md §3.1).
    ///
    /// \return Typed function pointer (NotifyLibChangedFn).
    /// \throws OrlibResolverError if ordinal 57 is not in the export table.
    NotifyLibChangedFn resolveNotifyLibChanged();

    /// \brief Resolve Target 5: InitGlobalLookup (__cdecl).
    /// RVA resolved at runtime via signature scan (see
    /// signatures/v9.50.01.3344.json).
    /// \return Typed function pointer (InitGlobalLookupFn).
    /// \throws OrlibResolverError if the signature does not match uniquely.
    InitGlobalLookupFn resolveInitGlobalLookup();

    /// \brief Resolve Target 6: DeleteFromTree (__thiscall).
    /// RVA resolved at runtime via signature scan (see
    /// signatures/v9.50.01.3344.json).
    ///
    /// Returns void* because MSVC has no __thiscall function-pointer type.
    /// The caller invokes it via inline asm (see callDeleteFromTree in
    /// tsdev_lib.cpp). The signature is scanned; the address is returned
    /// as an untyped pointer.
    ///
    /// \return Raw function pointer (void* — invoke via inline asm).
    /// \throws OrlibResolverError if the signature does not match uniquely.
    void* resolveDeleteFromTree();

    /// \brief Resolve Target 7: GetWarehouse (__cdecl).
    /// RVA resolved at runtime via signature scan (see
    /// signatures/v9.50.01.3344.json).
    ///
    /// Note: resolved and stored but NOT called in the current production
    /// flow — the delete orchestrator (FUN_101a8360) calls it internally.
    /// Retained as a resolved-but-unused CALL_TARGET.
    ///
    /// \return Typed function pointer (GetWarehouseFn).
    /// \throws OrlibResolverError if the signature does not match uniquely.
    GetWarehouseFn resolveGetWarehouse();

    /// \brief Resolve Target 8: FUN_101a8360 delete orchestrator (__fastcall).
    /// RVA resolved at runtime via signature scan (see
    /// signatures/v9.50.01.3344.json).
    ///
    /// Returns void* because this function is invoked via inline asm
    /// (ECX=whType, EDX=progId — see callDeleteOrchestrator in tsdev_lib.cpp).
    ///
    /// \return Raw function pointer (void* — invoke via inline asm).
    /// \throws OrlibResolverError if the signature does not match uniquely.
    void* resolveDeleteOrchestrator();

    /// \brief Resolve Target 9: FUN_101d43a0 type-array builder (__stdcall).
    /// RVA resolved at runtime via signature scan (see
    /// signatures/v9.50.01.3344.json).
    ///
    /// Returns void* because this function is invoked via inline asm
    /// (stdcall: 2 stack args — category, outArray — see callBuildTypeArray
    /// in tsdev_lib.cpp).
    ///
    /// \return Raw function pointer (void* — invoke via inline asm).
    /// \throws OrlibResolverError if the signature does not match uniquely.
    void* resolveBuildTypeArray();

    /// \brief Resolve Target 10: FUN_1000dd30 CELProgramWriter constructor
    /// (__fastcall: ECX=this/buffer). RVA resolved at runtime via signature
    /// scan (see signatures/v9.50.01.3344.json).
    ///
    /// Returns void* because this function is invoked via inline asm
    /// (fastcall: ECX=buffer — see callCelProgramWriterCtor in
    /// tsdev_lib.cpp). Used by refreshCache() to initialize the output
    /// buffer before calling FUN_101d0950 (cmd 0x0F dispatch).
    ///
    /// \return Raw function pointer (void* — invoke via inline asm).
    /// \throws OrlibResolverError if the signature does not match uniquely.
    void* resolveCelProgramWriterCtor();

    /// \brief Resolve FUN_101a42d0 — find item by name + get verbatim stored
    /// name (__fastcall: ECX=searchName, EDX=componentType, stack=warehouse,
    /// outVerbatimName; RET 0x8). RVA resolved at runtime via signature scan
    /// (see signatures/v9.50.01.3344.json).
    ///
    /// This is the official TSDev function for finding an item and getting
    /// its verbatim stored name. In server mode (DAT_1023aa8f == 1), it
    /// performs a BST search and lazily fetches the verbatim name from the
    /// server via command 0x34. The stored name comes from the WAREHOUSE
    /// SERVER, not from source text (HARD RULE 0).
    ///
    /// \return Raw function pointer (void* — invoke via inline asm).
    /// \throws OrlibResolverError if the signature does not match uniquely.
    void* resolveFindItemGetName();

    /// \brief Resolve FUN_101d05e0 — BST search wrapper (non-server mode)
    /// (__thiscall: ECX=warehouse, stack=(typeFilter, name, *outNode);
    /// RET 0xC). Returns bool (1=found, 0=not found). Stores BST node
    /// pointer in *outNode.
    ///
    /// \return Raw function pointer (void* — invoke via inline asm).
    /// \throws OrlibResolverError if the signature does not match uniquely.
    void* resolveBstSearchWrapper();

    /// \brief Resolve FUN_101c36d0 — verbatim name getter
    /// (__fastcall: ECX=node pointer, no stack args; RET). Returns char*
    /// to the verbatim stored name at node+0x1D.
    ///
    /// \return Raw function pointer (void* — invoke via inline asm).
    /// \throws OrlibResolverError if the signature does not match uniquely.
    void* resolveVerbatimNameGetter();

    // --- Export resolution (tracked exports plus ad-hoc diagnostic exports;
    //     see docs/SIGNATURE_CATALOG.md for the current tracked-export count) ---
    //
    // Exports are resolved by GetProcAddress (production) or by parsing the
    // PE export table from the in-memory buffer (test). These are NOT
    // signature-scanned — the export table provides the address directly.
    // The decorated names match src/tsdev_lib.cpp lines 367-377.
    // ------------------------------------------------------------------------

    /// \brief Resolve export: _OrlibInit@8 (__stdcall).
    /// \throws OrlibResolverError if GetProcAddress / export-table parse fails.
    OrlibInitFn resolveOrlibInit();

    /// \brief Resolve export: _OrlibExit@0 (__stdcall).
    /// \throws OrlibResolverError if GetProcAddress / export-table parse fails.
    OrlibExitFn resolveOrlibExit();

    /// \brief Resolve export: FindExistPos (UNDecorated, __stdcall).
    ///
    /// Note: orlib20.dll exports FindExistPos UNDECORATED (not
    /// _FindExistPos@8). Confirmed via dumpbin /exports
    /// (docs/SIGNATURE_CATALOG.md §5).
    /// \throws OrlibResolverError if GetProcAddress / export-table parse fails.
    FindExistPosFn resolveFindExistPos();

    /// \brief Resolve export: _CreateEntryEX@24 (__stdcall).
    /// \throws OrlibResolverError if GetProcAddress / export-table parse fails.
    CreateEntryEXFn resolveCreateEntryEX();

    /// \brief Resolve export: _SaveModule_New@20 (__stdcall).
    /// \throws OrlibResolverError if GetProcAddress / export-table parse fails.
    SaveModule_NewFn resolveSaveModuleNew();

    /// \brief Resolve export: _VerifyModule_New@20 (__stdcall).
    /// \throws OrlibResolverError if GetProcAddress / export-table parse fails.
    VerifyModule_NewFn resolveVerifyModuleNew();

    /// \brief Resolve export: _CloseWarehouseForCurrentThreadAndPath@4 (__stdcall).
    /// \throws OrlibResolverError if GetProcAddress / export-table parse fails.
    CloseWarehouseFn resolveCloseWarehouse();

    /// \brief Generic export resolution by decorated name.
    ///
    /// Used internally by the specific resolve* methods above. Also available
    /// publicly for resolving ad-hoc exports.
    ///
    /// \param decoratedName Export name as it appears in the export table
    ///                      (e.g. "_OrlibInit@8", "FindExistPos").
    /// \return Raw FARPROC (caller casts to the appropriate typedef).
    /// \throws OrlibResolverError if the export is not found.
    FARPROC resolveExport(const char* decoratedName);

    /// \brief Generic export resolution by ordinal number.
    ///
    /// Workstream M slices M.1/M.3 use ordinal-only ([NONAME]) exports for
    /// the CELCompiler alloc/destroy wrappers (66/67) and the 0x40A sender
    /// (57). These have NO name in the export name table, so resolveExport(name)
    /// cannot find them. This method resolves by ordinal directly:
    ///   - Production mode: GetProcAddress(m_module, MAKEINTRESOURCEA(ordinal))
    ///     (the Windows loader accepts ordinal-by-MAKEINTRESOURCEA lookup).
    ///   - Test mode: index into the export directory's AddressOfFunctions
    ///     array by (ordinal - export->Base), skipping the name array loop.
    ///     Validates the ordinal is in range [Base, Base + NumberOfFunctions).
    ///
    /// \param ordinal Export ordinal (e.g. 66, 67, 57). Must be >= 1.
    /// \return Raw FARPROC (caller casts to the appropriate typedef).
    /// \throws OrlibResolverError if the ordinal is out of range or the
    ///         export function RVA is 0 (empty / forwarder slot).
    FARPROC resolveExportByOrdinal(int ordinal);

    // --- Patch-site resolution & validation (current PATCH_SITEs; count:
    //     see docs/SIGNATURE_CATALOG.md) -------------------------------------
    //
    // Each current patch-site entry is marked in the signature catalog. The
    // signature matches the containing
    // function entry; the patch byte is at sigMatchRva + patchOffset.
    // The resolver validates that the bytes at the patch site match the
    // expected original bytes BEFORE allowing a patch to be applied.
    // ------------------------------------------------------------------------

    /// \brief Resolve a patch site by signature scan and return its descriptor.
    ///
    /// Scans the containing function's signature, computes the patch-site
    /// address (sigMatchRva + patchOffset), and returns a PatchSiteDescriptor
    /// with the patch RVA, original bytes, and replacement bytes.
    ///
    /// Does NOT validate original bytes yet — call validatePatchSite() next.
    ///
    /// \param name Patch-site name (e.g. "FUN_101d0950_thread_check_INT3").
    /// \return PatchSiteDescriptor with all patch metadata.
    /// \throws OrlibResolverError if the containing-function signature does
    ///         not match uniquely.
    PatchSiteDescriptor resolvePatchSite(const std::string& name);

    /// \brief Validate that the bytes at a patch site match the expected
    ///        original bytes.
    ///
    /// Reads `expectedBytes.size()` bytes at `baseAddress() + rva` and
    /// compares them (with wildcard mask support for address-dependent
    /// bytes) against `expectedBytes`. This is the pre-patch safety check:
    /// if the bytes don't match, the patch must NOT be applied (the DLL is
    /// a different build than expected).
    ///
    /// Wildcard handling: bytes in `expectedBytes` that correspond to a 0x00
    /// entry in `mask` are skipped (treated as ??). This accommodates
    /// address-dependent bytes (e.g. the low 2 bytes of an SEH handler
    /// address, which are image-base-relative and ASLR-dependent) when a
    /// patch-site signature requires wildcarding them.
    ///
    /// \param name          Patch-site name (for diagnostics).
    /// \param rva           Patch-site RVA (where the bytes live).
    /// \param expectedBytes Expected original bytes at the patch site.
    /// \param mask          Per-byte mask: 0xFF = check, 0x00 = wildcard.
    ///                       Must be same length as expectedBytes, or empty
    ///                       (meaning "check all bytes").
    /// \return true if all non-wildcard bytes match; false otherwise.
    /// \throws OrlibResolverError if the patch-site address is outside the
    ///         .text section (defensive — shouldn't happen if
    ///         resolvePatchSite was used).
    bool validatePatchSite(const std::string& name,
                           uintptr_t rva,
                           const std::vector<uint8_t>& expectedBytes,
                           const std::vector<uint8_t>& mask);

    /// \brief Validate a patch site using its resolved descriptor.
    ///
    /// Convenience overload: calls validatePatchSite with the original bytes
    /// and a wildcard mask derived from the signature definition. The
    /// thread-check INT3 patches are single-byte (0xCC) and require no
    /// wildcards; the descriptor-supplied original bytes are checked in full.
    ///
    /// \param desc PatchSiteDescriptor from resolvePatchSite().
    /// \return true if the patch site contains the expected original bytes.
    bool validatePatchSite(const PatchSiteDescriptor& desc);

    /// \brief Apply a patch: validate original bytes, then write replacement
    ///        bytes (with VirtualProtect).
    ///
    /// This is the full patch operation:
    ///   1. Resolve the patch site (resolvePatchSite).
    ///   2. Validate original bytes (validatePatchSite) — throw on mismatch.
    ///   3. Check idempotency: if replacement bytes are already present,
    ///      skip (return true).
    ///   4. VirtualProtect to PAGE_EXECUTE_READWRITE.
    ///   5. memcpy replacement bytes.
    ///   6. Restore original protection.
    ///
    /// In test mode (in-memory buffer), steps 4-6 are no-ops (the buffer is
    /// read-only); only validation is performed. This lets tests verify that
    /// patch sites contain expected bytes without actually patching.
    ///
    /// \param name Patch-site name (e.g. "FUN_101d0950_thread_check_INT3").
    /// \return true if the patch was applied (or was already applied).
    /// \throws OrlibResolverError if the signature doesn't match, the
    ///         original bytes don't match, or VirtualProtect fails.
    bool applyPatch(const std::string& name);

    // --- Specific patch-site convenience methods ---------------------------

    /// \brief Resolve & validate Target 10: FUN_101d0950 thread-check INT3.
    ///
    /// Containing function and patch RVAs are resolved at runtime via
    /// signature scan (see signatures/v9.50.01.3344.json); patch at offset
    /// 0x43. Original byte: 0xCC (INT3). Replacement: 0x90 (NOP). Command
    /// 0x0F (load program) dispatch helper.
    /// \return PatchSiteDescriptor for this thread-check patch.
    PatchSiteDescriptor resolveThreadCheck1();

    /// \brief Resolve & validate Target 11: FUN_101cf5b0 thread-check INT3.
    ///
    /// Containing function and patch RVAs are resolved at runtime via
    /// signature scan (see signatures/v9.50.01.3344.json); patch at offset
    /// 0x24. Original byte: 0xCC. Replacement: 0x90. General dispatch helper.
    /// \return PatchSiteDescriptor for this thread-check patch.
    PatchSiteDescriptor resolveThreadCheck2();

    /// \brief Resolve & validate Target 12: FUN_101d1fa0 thread-check INT3.
    ///
    /// Containing function and patch RVAs are resolved at runtime via
    /// signature scan (see signatures/v9.50.01.3344.json); patch at offset
    /// 0x28. Original byte: 0xCC. Replacement: 0x90. Command 0x17
    /// (save/commit) dispatch helper.
    /// \return PatchSiteDescriptor for this thread-check patch.
    PatchSiteDescriptor resolveThreadCheck3();

    // --- Data-section RVA resolution (3 data globals) ----------------------
    //
    // Data-section RVAs cannot be matched by instruction-pattern signatures
    // (there are no instructions in data). The resolution strategy
    // (docs/SIGNATURE_CATALOG.md §6 "Resolution strategy") is:
    //   1. Find the referencing code function by its code signature.
    //   2. Extract the data reference (4-byte absolute address operand)
    //      from the disassembled instruction.
    //   3. Convert the absolute address to an RVA (subtract image base).
    //
    // These methods are declared here for completeness; implementation is
    // TVS-B.4. They return the RVA of the data global (caller adds
    // baseAddress() to get the absolute address).
    // ------------------------------------------------------------------------

    /// \brief Resolve the error-record array pointer global (DAT_1023cea0).
    ///
    /// Referenced by StartCompilingProcess (Target 1) via
    /// `MOV ECX, [<error array ptr>]`. The 4-byte absolute address
    /// operand is extracted from that instruction and converted to an RVA.
    /// See signatures/v9.50.01.3344.json for the data-global RVAs.
    ///
    /// \return RVA of the error-record array pointer global.
    /// \throws OrlibResolverError if the referencing function signature
    ///         fails or the data reference cannot be extracted.
    uintptr_t resolveErrorArrayPtrRva();

    /// \brief Resolve the error-record count global (DAT_1023cea4).
    ///
    /// Referenced by StartCompilingProcess (Target 1) via
    /// `MOV [<error count>], EAX`. Extracted from the same function.
    /// See signatures/v9.50.01.3344.json for the data-global RVAs.
    /// \return RVA of the error-record count global.
    /// \throws OrlibResolverError on resolution failure.
    uintptr_t resolveErrorCountRva();

    /// \brief Resolve the string-table HINSTANCE global (DAT_1023aab8).
    ///
    /// Referenced by _CreateEntryEX@24 (resolved export) at instruction
    /// offset 0x6C via `MOV ECX, [<HINSTANCE>]` (opcode 8B 0D). The 4-byte
    /// absolute address operand is extracted from that instruction and
    /// converted to an RVA. See signatures/v9.50.01.3344.json for the
    /// data-global RVAs.
    ///
    /// \return RVA of the HINSTANCE global.
    /// \throws OrlibResolverError if the export resolution fails or the
    ///         data reference cannot be extracted.
    uintptr_t resolveErrorHinstRva();

    // --- Accessors ---------------------------------------------------------

    /// \brief Get the orlib20.dll base address (HMODULE cast to uintptr_t).
    ///
    /// In production mode, this is the loaded DLL's base. In test mode,
    /// this is the `base` passed to the test constructor. RVAs from
    /// resolveBySignature are relative to this base.
    uintptr_t baseAddress() const;

    /// \brief Get the raw HMODULE (production mode only).
    /// \return HMODULE, or nullptr in test mode.
    HMODULE moduleHandle() const;

    /// \brief Get the .text section bounds (RVA + size) as parsed from PE.
    /// \return Pair of {textRva, textRawSize}.
    std::pair<uintptr_t, size_t> textSectionBounds() const;

    /// \brief Is the resolver in test mode (in-memory buffer)?
    bool isTestMode() const;

    /// \brief Get the selected signature set (from detectVersion).
    SignatureSetId signatureSet() const;

protected:
    // --- Test seam: signature scanning -------------------------------------
    //
    // The core scan is a protected virtual method so subclasses (test
    // harnesses) can override it to inject fake scan behavior, fuzz the
    // matcher, or short-circuit specific targets.
    // ------------------------------------------------------------------------

    /// \brief Scan the .text section for a signature, returning all match RVAs.
    ///
    /// This is the core scan primitive. It iterates over the .text raw bytes
    /// and collects every offset where the non-wildcard bytes match. The
    /// public resolveBySignature() calls this and enforces uniqueness.
    ///
    /// \param sig  Signature definition (bytes, mask, length).
    /// \return Vector of match RVAs (empty if no match).
    virtual std::vector<uintptr_t> scanTextSection(const OrlibSignatureDef& sig);

    /// \brief Parse PE headers from the image (loaded or in-memory buffer).
    ///
    /// Extracts: image base, .text section RVA/virtual size/raw size/raw
    /// offset, export directory RVA/size. Called once at construction.
    /// \throws OrlibResolverError if the PE headers are invalid.
    virtual void parsePeHeaders();

    /// \brief Read the FileVersion string from the PE version resource.
    ///
    /// Uses GetFileVersionInfoW / VerQueryValueW (production) or parses the
    /// resource section from the in-memory buffer (test).
    /// \return FileVersion string (e.g. "9.50.01.3344").
    /// \throws OrlibResolverError if the version resource cannot be read.
    virtual std::string readFileVersion();

    /// \brief Compute SHA-256 of the on-disk DLL file.
    ///
    /// Uses BCryptCreateHash / BCryptHashData (or a self-contained SHA-256).
    /// Computes over the on-disk file (not the in-memory image) — see
    /// docs/VERSION_DETECTION.md §8.5. In test mode, hashes the in-memory
    /// buffer instead.
    /// \return Uppercase hex string (64 chars, no spaces).
    virtual std::string computeSha256();

private:
    // --- State -------------------------------------------------------------

    bool         m_testMode = false;  ///< True if operating on an in-memory buffer.
    HMODULE      m_module   = nullptr; ///< Loaded DLL handle (production only).
    uintptr_t    m_base     = 0;      ///< Image base address (loaded or simulated).
    const uint8_t* m_image  = nullptr; ///< In-memory PE image buffer (test only).
    size_t       m_imageSize = 0;     ///< Size of m_image buffer (test only).

    // --- Parsed PE metadata ---
    uintptr_t    m_textRva      = 0;  ///< .text section RVA.
    size_t       m_textVsize    = 0;  ///< .text virtual size.
    size_t       m_textRawSize  = 0;  ///< .text raw size.
    size_t       m_textRawOffset = 0; ///< .text raw file offset.
    uintptr_t    m_exportDirRva = 0;  ///< Export directory RVA.
    size_t       m_exportDirSize = 0; ///< Export directory size.

    // --- Version detection result (cached) ---
    VersionDescriptor m_version;      ///< Result of detectVersion().

    // --- Signature catalog (loaded at construction) ---
    //
    // The signature definitions from signatures.gen.h, converted to
    // OrlibSignatureDef form (byte arrays + masks); see
    // docs/SIGNATURE_CATALOG.md for the current signature-definition count.
    // The implementation (TVS-B.4) populates this from kSignatures[] or from
    // a per-version signature set selected by detectVersion().
    std::vector<OrlibSignatureDef> m_signatures;

    /// \brief Look up a signature by target name.
    /// \throws OrlibResolverError if the name is not in the catalog.
    const OrlibSignatureDef& findSignature(const std::string& name) const;

    /// \brief Resolve an export by parsing the PE export table (test mode).
    ///
    /// In test mode (no HMODULE), GetProcAddress is unavailable. This method
    /// walks the PE export directory in the in-memory buffer to find the
    /// export by name.
    /// \return FARPROC (address within the in-memory buffer).
    /// \throws OrlibResolverError if the export is not found.
    FARPROC resolveExportFromBuffer(const char* decoratedName);

    /// \brief Resolve an export by ordinal from the in-memory PE buffer
    /// (test mode).
    ///
    /// Workstream M slice M.1: ordinal-indexed path that reads
    /// funcRvas[ordinal - Base] directly, skipping the name array loop. Used
    /// by resolveExportByOrdinal() in test mode for ordinal-only ([NONAME])
    /// exports (e.g. orlib20 ordinals 66/67) that have no name table entry.
    /// Validates the ordinal is in range [Base, Base + NumberOfFunctions) and
    /// that the function RVA is non-zero (a zero RVA is an empty slot or a
    /// forwarder, neither of which is a callable function).
    /// \return FARPROC (address within the in-memory buffer: base + RVA).
    /// \throws OrlibResolverError if the ordinal is out of range or the
    ///         function RVA is 0.
    FARPROC resolveExportFromBufferByOrdinal(int ordinal);
};

} // namespace tsdev

#endif // ORLIB_RESOLVER_H
