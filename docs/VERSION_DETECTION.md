# Version Detection — orlib20.dll

| Field | Value |
|---|---|
| **TVS Identifier** | TVS-B.2 |
| **Date** | 2026-08-17 |
| **Status** | APPROVED |
| **Purpose** | Primary deliverable of TVS-B.2. Investigates version-detection strategies for `orlib20.dll`, documents the baseline version indicators for the currently-tested DLL, assesses indicator stability and discriminating power, recommends a multi-factor version-detection method, and specifies a known-versions manifest format that maps version indicators to expected signatures. Feeds into the `OrlibResolver` design (TVS-B.3) and implementation (TVS-B.4/B.5). |
| **Input artifacts** | `tmp/re_artifacts/b2_version_indicators.md` (primary), `tmp/re_artifacts/b1_dll_baseline.md` (cross-reference), `docs/SIGNATURE_CATALOG.md` (B.1 signature catalog), `docs/HANDOFF_B_ROBUSTNESS.md` (workstream spec) |

---

## 1. Purpose & Scope

This document is the research deliverable for **TVS-B.2** of Workstream B (Robustness). It investigates how the tsdev_automation CLI should detect the version of the loaded `orlib20.dll` at runtime, so that the CLI can (a) confirm it is running against a known-good, tested DLL version and (b) emit a clear warning — not a silent crash — when the DLL is an untested version. The document delivers four things: a complete table of baseline version indicators extracted from the current DLL (§2), a per-indicator analysis of stability and discriminating power with a ranked recommendation (§3), a concrete multi-factor version-detection method with a decision tree and conservative fallback (§4), an honest variability assessment noting the single-version limitation (§5), and a recommended known-versions manifest format with exact C++ and JSON definitions (§6). Integration notes for downstream TVSs are in §7; limitations and open questions are in §8. This is a design document only — no code is changed, no implementation is produced.

---

## 2. Baseline Version Indicators

*Source: `tmp/re_artifacts/b2_version_indicators.md` (primary), cross-checked against `tmp/re_artifacts/b1_dll_baseline.md` and `docs/SIGNATURE_CATALOG.md` §1. All measurements taken from the read-only install original at `C:\Program Files (x86)\TradeStation 9.5\Program\orlib20.dll`. No discrepancies found between B.1 and B.2 extractions.*

### 2.1 PE version resource (VS_VERSIONINFO)

| Field | Value | Extraction method |
|---|---|---|
| **FileVersion** | `9.50.01.3344` | `[System.Diagnostics.FileVersionInfo]::GetVersionInfo()` / `Get-Item .VersionInfo` |
| **FileVersionRaw** | `9.50.1.3344` | (same) |
| **ProductVersion** | `9.50.01.3344` | (same) |
| **ProductVersionRaw** | `9.50.1.3344` | (same) |
| **ProductName** | `EasyLanguage` | (same) |
| **InternalName** | `OR_LIB` | (same) |
| **OriginalFilename** | `OR_LIB.DLL` | (same) |
| **CompanyName** | `TradeStation Technologies, Inc.` | (same) |
| **FileDescription** | `Omega EasyLanguage` | (same) |
| **LegalCopyright** | `Copyright c TradeStation Technologies, Inc.? All Rights Reserved` | (same) |
| **FileMajorPart** | `9` | (same) |
| **FileMinorPart** | `50` | (same) |
| **FileBuildPart** | `1` | (same) |
| **FilePrivatePart** | `3344` | (same) |
| **Language** | `English (United States)` | (same) |

### 2.2 PE header & file indicators

| Indicator | Value | Extraction method |
|---|---|---|
| **PE timestamp (COFF TimeDateStamp)** | `0x5C01735E` = Fri Nov 30 17:29:02 2018 UTC | PowerShell PE-header byte parse (offset `0x3C` → COFF +8); cross-checked via `dumpbin /headers` |
| **File size** | 4,924,400 bytes (`0x4B23F0`) | `(Get-Item $dll).Length` |
| **SHA-256** | `318DB24A4F41BDB9FF69E2E647026B788706904CF887CBB0A953E003DB22EC18` | `Get-FileHash -Algorithm SHA256` |
| **Machine** | `0x014C` (x86) | `dumpbin /headers` |
| **Characteristics** | `0x2102` (Executable, 32-bit word machine, DLL) | `dumpbin /headers` |
| **Magic** | `0x010B` (PE32) | `dumpbin /headers` |
| **Linker version** | 14.00 | `dumpbin /headers` |
| **Entry point (RVA)** | `0x1EB235` (VA `0x101EB235`) | `dumpbin /headers` |
| **Image base** | `0x10000000` (range `0x10000000`–`0x104BAFFF`) | `dumpbin /headers` |

### 2.3 .text section

| Field | Value | Extraction method |
|---|---|---|
| **Section name** | `.text` | PowerShell PE section-header parse; cross-checked `dumpbin /headers` |
| **Virtual address (RVA)** | `0x00001000` | (same) |
| **Virtual size** | `0x001F72FC` (2,061,052 bytes) | (same) |
| **Raw size (size of raw data)** | `0x001F7400` (2,061,312 bytes) | (same) |
| **File pointer to raw data** | `0x00000400` | (same) |

### 2.4 Export table

| Field | Value | Extraction method |
|---|---|---|
| **Number of functions (export address table entries)** | 699 | PowerShell PE export-directory parse; cross-checked `dumpbin /exports` |
| **Number of names (named exports)** | 636 | (same) |
| **Ordinal base** | 10 | (same) |
| **Highest ordinal** | 708 | (same) |
| **Lowest ordinal** | 10 | (same) |
| **Export directory RVA** | `0x21CF90` | (same) |
| **Export directory size** | `0x6B30` | (same) |
| **Export directory TimeDateStamp** | `0x5C017315` (Fri Nov 30 17:27:49 2018 — distinct from PE header timestamp) | (same) |

**Note:** 699 address-table entries vs. 636 named exports = 63 exports exported by ordinal only (no name). Ordinals span 10–708 with a few gaps (e.g., ordinal 704 absent from the named list).

### 2.5 Baseline summary (one-liner)

Version `9.50.01.3344` (Omega EasyLanguage, TradeStation Technologies, Inc.), 4,924,400 bytes, SHA-256 `318DB24A4F41BDB9FF69E2E647026B788706904CF887CBB0A953E003DB22EC18`, PE timestamp `0x5C01735E` (Fri Nov 30 17:29:02 2018 UTC), image base `0x10000000`, .text RVA `0x00001000` / vsize `0x1F72FC` / raw `0x1F7400`, 699 exports / 636 named / highest ordinal 708 / ordinal base 10.

---

## 3. Indicator Analysis — Stability & Discriminating Power

Each indicator is assessed on three axes: **(a) stability** — how likely it stays constant across minor rebuilds of the *same* version; **(b) discriminating power** — how well it distinguishes *different* versions; **(c) extraction cost** — how expensive to read at runtime.

### 3.1 Per-indicator assessment

| Indicator | Stability (same-version rebuilds) | Discriminating power (different versions) | Extraction cost (runtime) | Notes |
|---|---|---|---|---|
| **FileVersion** (`9.50.01.3344`) | **High** — set by the build system's version stamp; stays constant across rebuilds of the same release. | **Medium** — distinguishes major/minor releases, but two different builds could share the same FileVersion if not bumped. Can be manually edited (spoofable). | **Low** — read from the PE version resource via `GetFileVersionInfoW` / `VerQueryValueW` (Win32 API, no file hashing). | Human-readable; the natural primary key for a version manifest. |
| **PE timestamp** (`0x5C01735E`) | **Low** — changes on every build (it is the linker's `time()` at link time). A rebuild with no code change still gets a new timestamp. | **High** — almost certainly unique per build. | **Low** — read 4 bytes at COFF header offset (PE offset + 8). No file hashing. | Excellent as a secondary cross-check to catch rebuilds that share a FileVersion. |
| **File size** (4,924,400) | **Medium-High** — stable across rebuilds that don't change code/data size, but changes if any section grows. | **Low** — two functionally different versions could coincidentally have the same file size. | **Very low** — `GetFileSize` or `GetFileInformationByHandle`. | Weak discriminator; useful only as a cheap "something changed" signal. |
| **SHA-256** (`318DB24A…EC18`) | **Low** — changes on ANY byte change, including non-functional rebuilds (timestamp, padding, resource edits). | **Highest** — definitive byte-identity. No two distinct files share a SHA-256 (collision infeasible). | **High** — must read and hash the entire 4.9 MB file. | The definitive identity check, but too expensive to use as the primary gate. Use it as a tertiary confirmation. |
| **.text virtual size** (`0x1F72FC`) | **Medium-High** — changes only if the code section grows or shrinks. | **Low** — same weakness as file size; two versions could have identical .text size. | **Low** — parse PE section headers (already needed for signature scanning). | More specific than file size (isolates code changes from data/resource changes) but still weak as a standalone discriminator. |
| **.text raw size** (`0x1F7400`) | **Medium-High** — same as virtual size, but rounded to file alignment. | **Low** — weaker than virtual size because alignment rounding can mask small changes. | **Low** — same parse as virtual size. | Redundant with virtual size for discrimination; include both in the manifest for completeness. |
| **Export count** (699 / 636) | **High** — changes only if exports are added or removed. | **Low** — many versions could have the same export count. | **Low** — parse PE export directory (already needed if validating exports). | Weak discriminator; useful as a sanity check (a wildly different export count signals a major version change). |
| **Image base** (`0x10000000`) | **High** — set by linker; constant for a given DLL unless reconfigured. | **Low** — most 32-bit DLLs use `0x10000000` as default. | **Very low** — read from optional header. | Near-useless as a discriminator, but cheap to record and can catch a fundamentally different binary. |
| **PE version resource metadata** (ProductName, InternalName, etc.) | **High** — static strings set once per product. | **Low** — constant across many versions of the same product line. | **Low** — same `VerQueryValueW` calls as FileVersion. | Useful for confirming "this is an orlib20.dll from TradeStation" (sanity check), not for version discrimination. |

### 3.2 Ranking by suitability for runtime version detection

| Rank | Indicator | Role | Rationale |
|---|---|---|---|
| 1 | **FileVersion** | Primary key | Fast, human-readable, stable across same-version rebuilds, natural manifest key. |
| 2 | **PE timestamp + file size** | Secondary cross-check | Cheap, catches rebuilds that share a FileVersion but differ in build. |
| 3 | **SHA-256** | Tertiary confirmation | Definitive byte-identity, but expensive — compute only after primary + secondary match. |
| 4 | **.text virtual size** | Tertiary cross-check | Already parsed for signature scanning; free additional signal. |
| 5 | **Export count** | Sanity check | Cheap; a large delta signals a major version change. |
| 6 | **Image base + version-resource metadata** | Sanity check | Near-free; confirms the binary is plausibly an orlib20.dll. |

### 3.3 Why single-factor detection is fragile

Relying on any single indicator is brittle:

- **FileVersion only:** Two different builds can share the same FileVersion if the vendor doesn't bump it. The version resource can also be manually edited (spoofed). A rebuild with the same version string but shifted code would pass the version check but fail signature scans — and the CLI would have no early warning.
- **SHA-256 only:** Changes on any byte change, including non-functional rebuilds (e.g., a vendor re-links with a newer linker, changing only the PE timestamp and padding). The CLI would reject a functionally-identical DLL as "unknown," producing false warnings. It is also expensive to compute on every startup.
- **PE timestamp only:** Changes on every build, so it cannot group "same version, different rebuild" — it would treat every rebuild as a new version, defeating the purpose of a version-keyed manifest.
- **File size only:** Two functionally different versions can have the same size; the CLI would silently treat a different version as known-good.

A **multi-factor** approach combines the strengths: FileVersion groups rebuilds of the same release (fast, human-readable), PE timestamp + file size catch rebuilds that share a version string (cheap), and SHA-256 confirms exact byte-identity with a known-good manifest entry (definitive, computed only when needed). This yields three meaningful outcomes — "known & verified," "known but rebuilt," and "unknown" — instead of a single binary pass/fail.

---

## 4. Recommended Version-Detection Method

### 4.1 Method: tiered multi-factor

The recommended method is a **three-tier check**, evaluated in order of increasing cost and specificity:

| Tier | Indicator(s) | Purpose | Cost | Action on match | Action on mismatch |
|---|---|---|---|---|---|
| **Tier 1 (primary)** | FileVersion string | Fast lookup in the known-versions manifest | Low | Proceed to Tier 2 | Version is **unknown** → §4.3 conservative fallback |
| **Tier 2 (secondary)** | PE timestamp + file size | Catch rebuilds with same FileVersion | Low | Proceed to Tier 3 | Version is **known but rebuilt** → warn, proceed cautiously (signatures may still match) |
| **Tier 3 (tertiary)** | SHA-256 | Confirm exact byte-identity with manifest entry | High | Report **"known & verified"** | Report **"known but byte-differs"** → warn, proceed cautiously |

**Rationale for the tiering:** Tier 1 is the cheap gate that handles the common case (same version, same build) without hashing the file. Tier 2 adds a cheap cross-check that catches the "same version string, different build" case — the most likely real-world scenario when a vendor ships a hotfix rebuild. Tier 3 is the definitive check, computed only when Tiers 1 and 2 already match, so the expensive hash is never computed for unknown versions or obvious mismatches.

### 4.2 Startup decision tree

The following describes how the CLI proceeds at startup (to be implemented in the `OrlibResolver` constructor, TVS-B.4/B.5):

```
1. Load orlib20.dll (LoadLibraryW); capture HMODULE as base address.
2. Read FileVersion from the PE version resource (GetFileVersionInfoW + VerQueryValueW).
3. Look up FileVersion in the known-versions manifest (constexpr array, §6):
   a. NOT FOUND → version is UNKNOWN.
      → Print stderr warning (§4.3).
      → Proceed to signature resolution (signatures may still match).
      → If signature resolution fails → throw std::runtime_error with clear message.
   b. FOUND → record the manifest entry; proceed to Tier 2.
4. Read PE timestamp (COFF TimeDateStamp) and file size from the loaded DLL.
   Compare against the manifest entry's peTimestamp and fileSize:
   a. MISMATCH → version is KNOWN BUT REBUILT.
      → Print stderr warning: "orlib20.dll version X — build indicators differ
        from tested build (expected timestamp T / size S, found T'/S');
        signatures may not match; proceeding at your own risk."
      → Proceed to signature resolution.
      → If signature resolution fails → throw std::runtime_error.
   b. MATCH → proceed to Tier 3.
5. Compute SHA-256 of the loaded DLL file (read from disk, not the in-memory image).
   Compare against the manifest entry's sha256:
   a. MATCH → report "orlib20.dll version X — KNOWN & VERIFIED (byte-identical)."
   b. MISMATCH → report "orlib20.dll version X — known version, byte-differs from
      tested build; signatures may not match; proceeding at your own risk."
   → Proceed to signature resolution in all cases.
6. Run signature resolution (all 12 targets from SIGNATURE_CATALOG.md).
   If any signature fails to resolve (scan count != 1) → throw std::runtime_error
   with the signature name, expected RVA, and match count.
```

**Key design property:** Version detection never *aborts* the CLI on its own. It only produces warnings. The actual hard failure comes from signature resolution (TVS-B.4) — if the signatures don't match, the CLI throws a clear error rather than calling into the wrong code. This separates "version is untested" (a warning) from "signatures don't match" (a hard error), which is the correct separation of concerns: an untested version might still work, but a failed signature scan definitely won't.

### 4.3 Conservative fallback for unknown versions

When the FileVersion is not found in the manifest (Tier 1 miss), the CLI prints a clear warning to **stderr** and proceeds:

```
tsdev: WARNING — UNTESTED orlib20.dll version 9.50.01.3400.
    This version is not in the known-versions manifest (tested: 9.50.01.3344).
    Signatures may not match; proceeding at your own risk.
    If signature resolution fails, the CLI will abort with an error.
```

The CLI does **not** abort at this stage — the signatures may still match (a minor rebuild often preserves prologue byte patterns). If signature resolution subsequently fails, the CLI throws a `std::runtime_error` naming the failed signature and the match count. This is the conservative behavior mandated by Risk R6 (§5): unknown versions get a **warning, not a silent proceed**, and a failed signature scan is a **hard error, not a crash**.

---

## 5. Variability Assessment

### 5.1 Single-version limitation (HONEST STATEMENT)

**Only one `orlib20.dll` version exists on this system:** FileVersion `9.50.01.3344`, SHA-256 `318DB24A4F41BDB9FF69E2E647026B788706904CF887CBB0A953E003DB22EC18`, PE timestamp `0x5C01735E` (Fri Nov 30 17:29:02 2018 UTC), file size 4,924,400 bytes. A recursive search of all TradeStation install directories (`C:\Program Files*\TradeStation*`) and the user profile found exactly one copy, at the known install path. No 64-bit TradeStation directory exists. No backup or update directories contain an `orlib20.dll`. The offline analysis copy (`tests/fixtures/orlib20_baseline.dll`) is byte-identical (same hash, same size).

**No cross-version signature-stability comparison is possible.** The signature-stability comparison table required by the TVS-B.2 acceptance criteria cannot be produced because there is no second version to compare against. This limitation is fundamental, not an oversight.

### 5.2 Implication: signature stability across versions is UNVERIFIED

The 12 signatures in `docs/SIGNATURE_CATALOG.md` were derived from and verified against exactly one `orlib20.dll` build. Whether these signatures remain unique and correctly anchored in other versions is **unknown**. The design must therefore be **conservative**:

- Any version not in the manifest is treated as "untested — proceed at your own risk" (§4.3).
- Signature resolution failure is a hard error (§4.2, step 6).
- The manifest uses minimal-length signatures (per Risk R6 mitigation) to reduce assumptions about surrounding code, but this does not guarantee cross-version stability.

### 5.3 Risk R6 (from HANDOFF_B §6)

> **R6 — Only one orlib20.dll version is available for analysis.** Likelihood: High. Impact: Medium — signature stability across versions is unverified. **Mitigation:** TVS-B.2 explicitly documents this limitation. The design is conservative: unknown versions get a warning, not a silent proceed. The signature catalog uses minimal-length signatures (fewer assumptions about surrounding code).

This document is the TVS-B.2 deliverable that fulfills the "explicitly documents this limitation" mitigation. The conservative fallback (§4.3) fulfills the "unknown versions get a warning" mitigation. The minimal-length signature strategy is documented in `docs/SIGNATURE_CATALOG.md` (TVS-B.1).

### 5.4 Recommended process when a new version appears

When a new `orlib20.dll` version becomes available (e.g., via a TradeStation update), the following process should be followed:

1. **Run the regression suite** (TVS-B.6) against the new DLL copy (placed in `tmp/`). The suite's signature/patch portion (`test_orlib_resolver`) runs without a live TS install.
2. **If all signatures resolve** and all patch sites contain expected original bytes → the new version is compatible. Add a new manifest entry (§6) with the new version's indicators. No new signatures needed.
3. **If signatures fail to resolve** → derive new signatures for the shifted functions (repeat the TVS-B.1 extraction process against the new DLL) and add a new manifest entry pointing to the new signature set. Update `docs/SIGNATURE_CATALOG.md` with a per-version section.
4. **If patch-site original bytes differ** → record the new original bytes in the manifest entry's patch metadata. The patch validation logic (TVS-B.5) reads expected bytes from the manifest, so this is a data change, not a code change.

This process ensures the manifest grows organically as new versions are encountered, and every entry is regression-validated before being marked as "known & verified."

---

## 6. Known-Versions Manifest Format

> **WORKSTREAM E NOTICE (2026-08-17):** The version manifest format has been migrated from a hand-coded `constexpr` array in `orlib_resolver.cpp` to a JSON file (`signatures/version_manifest.json`) consumed via build-time code generation. The `OrlibVersionManifestEntry` struct and 3-tier detection algorithm are UNCHANGED — only the data source changed from hand-coded to generated. See `docs/ARCHITECTURE_REDESIGN.md` §4 (Signature Data Architecture) for the JSON schema and codegen pipeline. This migration is **IMPLEMENTED (Workstream E complete)**.

### 6.1 Recommendation: C++ constexpr struct array (primary) + JSON (documentation)

> **SUPERSEDED (Workstream E):** The manifest is now implemented via JSON (`signatures/version_manifest.json`) → `sigcodegen.py` codegen → generated header (`signatures.gen.h`). The C++ constexpr approach described here was superseded by the codegen pipeline in Workstream E. The section below is retained as the original design rationale; the actual implementation follows the JSON + codegen approach documented in `docs/ARCHITECTURE_REDESIGN.md` §4.

**Primary format: a C++ `constexpr` struct array, compiled into the CLI binary.**

Rationale:
- **No JSON dependency at runtime.** The CLI is a standalone 32-bit C++ executable with minimal dependencies. Adding a JSON parser (even a header-only one) is unnecessary complexity for a small, slowly-changing manifest.
- **Type-safe.** Struct fields have explicit types (`uint32_t`, `const char*`); the compiler catches typos and type mismatches at build time.
- **Zero parsing cost.** The manifest is baked into the binary; no file I/O or parsing at startup.
- **Manifest is tied to a specific CLI build.** Each CLI release is tested against specific DLL versions; compiling the manifest in ensures the CLI binary and its tested-version knowledge are never out of sync. A loose JSON file could be edited independently of the binary, creating a mismatch between "what the CLI was tested against" and "what it claims to know."
- **Easy to extend.** Adding a version is a one-line array entry; the compiler enforces field completeness.

**Secondary format: a JSON document (`docs/known_versions.json`), maintained for human inspection and documentation.** This is not loaded at runtime; it mirrors the compiled-in array so that the manifest is readable without parsing C++ source. It can also serve as a source for code generation if the manifest grows large.

### 6.2 C++ struct definition

```cpp
// orlib_version_manifest.h — Known-versions manifest for orlib20.dll
//
// Compiled-in (constexpr) manifest mapping orlib20.dll version indicators
// to expected signature-set identifiers. Populated by TVS-B.2; consumed by
// the OrlibResolver (TVS-B.3/B.4/B.5).
//
// ONE entry per known-tested DLL version. Currently one entry (9.50.01.3344).

#include <cstdint>

namespace tsdev {

// Identifies which signature set to use for this version.
// Index into a separate signature table (defined in the resolver, TVS-B.4).
// For now, only one signature set exists (SIGSET_V9_50_01_3344).
enum SignatureSetId : uint16_t {
    SIGSET_NONE        = 0,   // invalid / unknown
    SIGSET_V9_50_01_3344 = 1, // the only tested version
};

struct OrlibVersionManifestEntry {
    // --- Tier 1: primary key (PE version resource) ---
    const char* fileVersion;       // e.g. "9.50.01.3344" (human-readable, manifest key)

    // --- Tier 2: secondary cross-check (cheap, catches rebuilds) ---
    uint32_t    peTimestamp;       // COFF TimeDateStamp (e.g. 0x5C01735E)
    uint32_t    fileSize;          // file size in bytes (e.g. 4924400)

    // --- Tier 3: tertiary confirmation (definitive, expensive) ---
    const char* sha256;            // uppercase hex, 64 chars, no spaces (e.g. "318DB24A...EC18")

    // --- Additional indicators (sanity checks, near-free) ---
    uint32_t    textVsize;         // .text virtual size (e.g. 0x001F72FC)
    uint32_t    textRawSize;       // .text raw size   (e.g. 0x001F7400)
    uint32_t    textRva;           // .text RVA        (e.g. 0x00001000)
    uint32_t    imageBase;         // optional header ImageBase (e.g. 0x10000000)
    uint16_t    exportCount;       // NumberOfFunctions (e.g. 699)
    uint16_t    exportNamedCount;  // NumberOfNames     (e.g. 636)
    uint16_t    exportHighestOrdinal; // e.g. 708
    uint16_t    exportOrdinalBase;    // e.g. 10

    // --- PE version resource metadata (sanity / human-readable) ---
    const char* productVersion;    // e.g. "9.50.01.3344"
    const char* productName;       // e.g. "EasyLanguage"
    const char* fileDescription;   // e.g. "Omega EasyLanguage"
    const char* companyName;       // e.g. "TradeStation Technologies, Inc."
    const char* internalName;      // e.g. "OR_LIB"
    const char* originalFilename;  // e.g. "OR_LIB.DLL"

    // --- Signature set reference ---
    SignatureSetId signatureSet;   // which signature set to use for this version
};

// The manifest: one entry per known-tested orlib20.dll version.
// Add new entries here after regression-validating against a new DLL (§5.4).
constexpr OrlibVersionManifestEntry kOrlibVersionManifest[] = {
    {
        // Tier 1
        "9.50.01.3344",
        // Tier 2
        0x5C01735E,    // peTimestamp (Fri Nov 30 17:29:02 2018 UTC)
        4924400,       // fileSize
        // Tier 3
        "318DB24A4F41BDB9FF69E2E647026B788706904CF887CBB0A953E003DB22EC18",
        // Additional indicators
        0x001F72FC,    // textVsize
        0x001F7400,    // textRawSize
        0x00001000,    // textRva
        0x10000000,    // imageBase
        699,           // exportCount
        636,           // exportNamedCount
        708,           // exportHighestOrdinal
        10,            // exportOrdinalBase
        // PE version resource metadata
        "9.50.01.3344",
        "EasyLanguage",
        "Omega EasyLanguage",
        "TradeStation Technologies, Inc.",
        "OR_LIB",
        "OR_LIB.DLL",
        // Signature set
        SIGSET_V9_50_01_3344,
    },
    // --- Future entries added here after regression validation ---
};

constexpr size_t kOrlibVersionManifestCount =
    sizeof(kOrlibVersionManifest) / sizeof(kOrlibVersionManifest[0]);

} // namespace tsdev
```

### 6.3 JSON equivalent (documentation only — not loaded at runtime)

```json
{
  "manifest_version": 1,
  "description": "Known-tested orlib20.dll versions. Mirrors the compiled-in constexpr array in orlib_version_manifest.h. NOT loaded at runtime — for human inspection only.",
  "entries": [
    {
      "fileVersion": "9.50.01.3344",
      "peTimestamp": "0x5C01735E",
      "peTimestampHuman": "Fri Nov 30 17:29:02 2018 UTC",
      "fileSize": 4924400,
      "fileSizeHex": "0x4B23F0",
      "sha256": "318DB24A4F41BDB9FF69E2E647026B788706904CF887CBB0A953E003DB22EC18",
      "textVsize": "0x001F72FC",
      "textRawSize": "0x001F7400",
      "textRva": "0x00001000",
      "imageBase": "0x10000000",
      "exportCount": 699,
      "exportNamedCount": 636,
      "exportHighestOrdinal": 708,
      "exportOrdinalBase": 10,
      "productVersion": "9.50.01.3344",
      "productName": "EasyLanguage",
      "fileDescription": "Omega EasyLanguage",
      "companyName": "TradeStation Technologies, Inc.",
      "internalName": "OR_LIB",
      "originalFilename": "OR_LIB.DLL",
      "signatureSet": "SIGSET_V9_50_01_3344",
      "signatureCatalogRef": "docs/SIGNATURE_CATALOG.md"
    }
  ]
}
```

### 6.4 How the manifest is used

The `OrlibResolver` (designed in TVS-B.3, implemented in TVS-B.4) uses the manifest as follows at construction time:

1. **Load the DLL** and capture the base address (`HMODULE`).
2. **Extract indicators** from the loaded DLL: FileVersion (via `GetFileVersionInfoW`/`VerQueryValueW`), PE timestamp + file size (via PE header parse / `GetFileSize`), .text section bounds (via PE section-header parse — already needed for signature scanning), and export count (via PE export-directory parse).
3. **Look up FileVersion** in `kOrlibVersionManifest` (linear scan — the array is tiny). If not found → unknown (§4.3). If found → record the matching entry.
4. **Compare Tier 2 indicators** (peTimestamp, fileSize) against the entry. If mismatch → known-but-rebuilt warning.
5. **Compare Tier 3 indicator** (SHA-256) against the entry — computed only if Tier 2 matched. If mismatch → known-but-byte-differs warning. If match → known & verified.
6. **Select the signature set** indicated by `entry.signatureSet` (e.g., `SIGSET_V9_50_01_3344`). The resolver then scans the .text section using the signatures from that set (referencing `docs/SIGNATURE_CATALOG.md`).

The manifest entry's `signatureSet` field is the link between version detection and signature-based resolution: each version maps to exactly one signature set, and each signature set is a complete collection of the 12 RVA-target signatures + 4 patch-site original-byte patterns from `docs/SIGNATURE_CATALOG.md`.

---

## 7. Integration Notes (for TVS-B.3 / B.4 / B.5)

This section notes handoff points only — it does not design the `OrlibResolver` (that is TVS-B.3's scope).

| Downstream TVS | Handoff point from this document |
|---|---|
| **TVS-B.3 (OrlibResolver design)** | The resolver interface must include a `detectVersion()` method that returns a structured version descriptor (known/unknown flag, matched manifest entry or null, tier-match level). The manifest struct (`OrlibVersionManifestEntry`, §6.2) and the tiered decision tree (§4.2) are inputs to the interface design. The resolver must expose the selected `SignatureSetId` so callers know which signature set is in use. |
| **TVS-B.4 (resolver implementation)** | The implementation reads the indicators exactly as described in §4.2 and §6.4. The `constexpr` manifest array is compiled into the resolver translation unit (or a dedicated `orlib_version_manifest.h`). The SHA-256 computation uses a standard hash (e.g., `BCryptCreateHash` / `BCryptHashData` with `BCRYPT_SHA256_ALGORITHM`, or a small self-contained SHA-256 implementation to avoid additional dependencies). Unit tests (`test_orlib_resolver.cpp`) must verify `detectVersion()` returns `known=true` for the current DLL and that a deliberately wrong FileVersion returns `known=false`. |
| **TVS-B.5 (TsDevLib integration)** | The `TsDevLib` constructor constructs an `OrlibResolver` first; if `detectVersion()` returns unknown, the constructor prints the stderr warning (§4.3) but does NOT throw — it proceeds to signature resolution. If signature resolution fails, the constructor throws `std::runtime_error`. The warning text format is specified in §4.3. The integration must not change the public `TsDevLib` API. |
| **TVS-B.6 (regression suite)** | The regression suite's `test_orlib_resolver` component validates version detection against a copied DLL in `tmp/`. When a new version appears (§5.4), the suite is the gate for adding a manifest entry. |
| **TVS-B.7 (path/constant externalization)** | The manifest's `signatureSet` field may eventually also carry structural constants (CELCompiler size, field offsets) per version. TVS-B.7 externalizes those constants; this document's manifest struct provides the per-version container. The `OrlibVersionManifestEntry` can be extended with a nested struct for structural constants in TVS-B.7 without breaking TVS-B.4. |

**Signature catalog reference:** Each manifest entry's `signatureSet` points to a set of signatures documented in `docs/SIGNATURE_CATALOG.md` (12 code-section RVA targets + 4 patch-site original-byte patterns + 7 export names). For now, only one version exists, so only one signature set (`SIGSET_V9_50_01_3344`) exists. When a new version is added (§5.4), either the same signature set is reused (if signatures still match) or a new `SignatureSetId` is created with new/updated signatures, and `docs/SIGNATURE_CATALOG.md` gains a per-version section.

---

## 8. Limitations & Open Questions

### 8.1 Single-version limitation (restated)

Only one `orlib20.dll` version (`9.50.01.3344`) is available on this system. Cross-version signature stability is **unverified**. The entire design is conservative on this basis (§5). This is the most significant limitation and is documented honestly per Risk R6.

### 8.2 No access to TradeStation update packages

No TradeStation update packages, beta builds, or alternate-version install media are available. The variability assessment is therefore limited to what can be observed on a single installation. If update packages become available, the process in §5.4 should be followed to expand the manifest.

### 8.3 PE version resource spoofing

The PE version resource (`VS_VERSIONINFO`) is a data structure in the DLL's resource section and can be manually edited with a resource editor (e.g., Resource Hacker). A malicious or careless actor could change the FileVersion string without changing the code. **This is an acceptable risk for this use case.** The CLI is not defending against adversarial DLLs (a hostile DLL could hook any function regardless of version detection). The goal is to detect *accidental* version mismatches — e.g., the user upgraded TradeStation and the CLI is now running against a new `orlib20.dll` it was never tested against. The Tier 2 (timestamp + size) and Tier 3 (SHA-256) cross-checks provide defense-in-depth: even if the FileVersion is spoofed, a mismatched timestamp/size/hash will trigger a "known but rebuilt" or "known but byte-differs" warning.

### 8.4 Open question: multiple manifest entries vs. single entry

**Question:** Should the CLI support multiple manifest entries at once, or only the one it was built against?

**Recommendation:** Support **multiple entries** (an array, as designed in §6.2). Currently populated with one entry (`9.50.01.3344`). Rationale:
- The array format is no harder to implement than a single struct.
- When a new version is regression-validated (§5.4), adding an entry is a one-line change — no structural refactor.
- A CLI build might legitimately be tested against multiple DLL versions (e.g., the current release and a beta).
- The linear scan over a 1–2 element array is negligible cost.

The alternative (single entry, compiled-in constants) would require a structural change when the second version appears. The array avoids that.

### 8.5 Open question: SHA-256 of on-disk file vs. in-memory image

**Question:** Should the Tier 3 SHA-256 be computed over the on-disk file or the in-memory loaded image?

**Recommendation:** Compute over the **on-disk file** (the path from which `LoadLibraryW` loaded the DLL). Rationale:
- The manifest's baseline hash (`318DB24A…EC18`) was computed over the on-disk file (§2.2). Comparing against an in-memory hash would require normalizing for relocations, which is complex and error-prone.
- The on-disk hash is the natural "identity" of the DLL build.
- The in-memory image differs from the on-disk file due to relocations, import resolution, and section padding — hashing it would produce a different value even for the same file.

The implementation should resolve the DLL's file path (e.g., via `GetModuleFileNameW(m_orlib, ...)`) and hash that file. This is a documentation note for TVS-B.4.

### 8.6 Open question: export directory timestamp

The export directory has its own TimeDateStamp (`0x5C017315`, 73 seconds before the PE header timestamp `0x5C01735E`). This is normal (two separate timestamps set at different stages of the build). It is recorded in the baseline (§2.4) but is **not** recommended as a version-detection indicator — it is redundant with the PE header timestamp and adds parsing complexity. It is included in the manifest for completeness only if desired; the tiered method (§4) does not use it.
