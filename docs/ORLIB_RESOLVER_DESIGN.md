# OrlibResolver Design Rationale

| Field | Value |
|---|---|
| **TVS Identifier** | TVS-B.3 |
| **Companion header** | `src/orlib_resolver.h` |
| **Status** | Design artifact (header interface only) |
| **Inputs** | `docs/SIGNATURE_CATALOG.md` (TVS-B.1), `docs/VERSION_DETECTION.md` (TVS-B.2), `tmp/re_artifacts/b1_signatures.h`, `src/tsdev_lib.h`, `src/tsdev_lib.cpp` |
| **Downstream** | TVS-B.4 (`.cpp` implementation), TVS-B.5 (TsDevLib integration), TVS-B.6 (regression suite) |

> **WORKSTREAM E NOTICE (2026-08-17):** The `OrlibResolver` class design, API, and algorithm are UNCHANGED by Workstream E. Only the signature data SOURCE changes: `tmp/re_artifacts/b1_signatures.h` (gitignored, hand-transcribed) is being replaced by `signatures/v9.50.01.3344.json` (version-controlled) → `sigcodegen.py` → generated `signatures.gen.h`. The `kSignatures[]` / `kSignatureCount` symbols consumed by `buildSignatureCatalog()` will be provided by the generated header instead of the tmp/ header. See `docs/ARCHITECTURE_REDESIGN.md` §4 for details. This migration is **DESIGNED but NOT YET IMPLEMENTED** — the current state (`b1_signatures.h` as the build dependency) remains the actual state until Workstream E's TVSs are executed.

---

## 1. Overview & Purpose

`OrlibResolver` is a single class that encapsulates **all** version-specific knowledge of `orlib20.dll` behind one interface, so that the rest of the CLI (the `TsDevLib` class) never touches a hardcoded RVA, a raw `HMODULE`, or a byte signature. The resolver's responsibilities, as declared in `src/orlib_resolver.h` lines 6-14, are:

1. **Load** `orlib20.dll` and capture the base address.
2. **Detect the DLL version** using the 3-tier method (TVS-B.2).
3. **Resolve every internal function** by signature scan (catalog TVS-B.1), with a fallback to a known-versions manifest.
4. **Validate patch sites** (expected original bytes) before patches are applied.
5. **Hand typed function pointers** to the caller.

### The problem it solves

The current production code (`src/tsdev_lib.cpp`) hardcodes 8 internal-function RVAs and 4 patch-site RVAs as compile-time constants (`kRvaStartCompiling`, `kRvaPersistencePatch`, etc., lines 50-95). These constants are valid only for the single tested build (FileVersion `9.50.01.3344`, PE timestamp `0x5C01735E`). If TradeStation ships a rebuild — even one with the same FileVersion — the linker may shift code, and every hardcoded RVA silently points at the wrong bytes. The result is a crash or, worse, a call into unrelated code that corrupts state.

`OrlibResolver` replaces those constants with **signature-based resolution** (byte patterns with wildcards) plus a **version manifest fallback**, making the CLI robust across DLL rebuilds that preserve the targeted instruction sequences.

### Role in the project

The resolver is an **internal refactor** of `TsDevLib`. It is not a new user-facing feature. The public `TsDevLib` API (`findExistPos`, `createEntry`, `saveSource`, `verifyModule`, `closeWarehouse`, `deleteEntry`, `modifyEntry`, `ellFileExists`, `getEllPath`, `getXmlPath`, `validatePersistencePatchBytes`) is unchanged. The integration (TVS-B.5) replaces the constructor's hardcoded RVA arithmetic and `GetProcAddress` calls with `OrlibResolver` calls.

---

## 2. Why Signature Scanning Over Hardcoded RVAs

### Rationale

Hardcoded RVAs are addresses expressed as fixed offsets from the DLL base. They are correct only for the exact build they were measured against. A signature, by contrast, locates a function by its **byte pattern** (prologue + distinctive instruction sequence) rather than by a fixed offset. As long as the function's prologue and structural instructions are preserved across a rebuild, the signature resolves to the correct address automatically.

The wildcard policy (documented in `docs/SIGNATURE_CATALOG.md` §2) is central to this stability. Wildcards (`??`) cover **only** address-dependent bytes:

| Category | Bytes wildcarded | Why |
|---|---|---|
| SEH handler addresses | 4 bytes at prologue offset 6-9 (or 13-16 for GetWarehouse) | Absolute, image-base-relative; change on relocation/rebuild |
| Data-section references | 4 bytes in `MOV reg, [imm32]` / `MOV [imm32], reg` | Absolute, image-base-relative pointers into `.data` |
| IAT indirect call targets | 4 bytes in `CALL [imm32]` (`FF 15 imm32`) | IAT slot addresses are image-base-relative |
| Relative call/jump targets | 4 bytes in `CALL rel32` (`E8 imm32`) and `JZ rel32` (`0F 8x imm32`) | Relative displacements change if code shifts |

Everything else — opcodes, ModR/M bytes, register encodings, structural immediates (stack frame sizes, error record stride/size, allocation sizes), short jump displacements — is **fixed** and must match exactly. This makes signatures stable under relocation and minor code shifts while remaining distinctive enough to be unique.

### Trade-offs

| Trade-off | Assessment |
|---|---|
| **Scan cost** | Signature scanning is O(text_size × sig_len) per target. The `.text` section is ~2 MB (`0x001F72FC` virtual size) and there are 12 signatures, so the full scan completes in milliseconds at startup. Acceptable for a CLI that runs once per invocation. |
| **Uniqueness requirement** | Each signature must match exactly once in `.text`. If a signature matches zero or multiple times, resolution fails (hard error). Uniqueness was verified: TVS-B.1's scan (`src/sig_scan.cpp` / `tmp/re_artifacts/b1_sig_scan_run.md`) confirmed **12/12 PASS, scan count = 1** for every target on the tested build. |
| **Fragility if code is refactored** | If TradeStation rewrites a targeted function (changing its prologue or structural instructions), the signature will no longer match. This is a hard error (not a silent crash), which is the correct behavior — the CLI reports the failure clearly rather than calling into the wrong code. The manifest fallback can supply recorded RVAs as a last resort for known versions (see §5). |
| **Single-version limitation** | Only one `orlib20.dll` version (`9.50.01.3344`) is available for analysis. Cross-version signature stability is **unverified** (Risk R6, `docs/VERSION_DETECTION.md` §5.3). The design is conservative on this basis: unknown versions get a warning, and signature resolution failure is a hard error. |

### Reference to TVS-B.1 verification

The 12 signatures in `tmp/re_artifacts/b1_signatures.h` were each verified to match exactly once at their expected RVA within the `.text` section of `orlib20_baseline.dll` (`docs/SIGNATURE_CATALOG.md` §8, result table). Target 12 required signature extension (from 41 to 57 bytes) to disambiguate a spurious second match at RVA `0x001CFCE0`; the final 57-byte signature is unique. This demonstrates the uniqueness-enforcement discipline: a multi-match is a signal to extend the signature, not to relax the requirement.

---

## 3. Class API Summary

The full interface is in `src/orlib_resolver.h` (931 lines). This section summarizes the public surface, grouped by concern. It does not reproduce the header; see the cited line ranges for exact signatures and documentation.

### 3.1 Supporting types (lines 215-358)

Before the class itself, the header declares the supporting types the resolver uses and returns:

| Type | Lines | Purpose |
|---|---|---|
| `OrlibSignatureDef` | 215-229 | A single byte signature with wildcard mask. Mirrors `SignatureDef` from `b1_signatures.h` but uses structured byte arrays (`bytes`, `mask`) instead of hex strings. Carries patch metadata (`originalBytes`, `replacementBytes`, `patchOffset`, `containingFunction`) for the 4 patch-site targets. |
| `SignatureSetId` (enum) | 237-240 | Identifies which signature set to use for a DLL version. `SIGSET_NONE` (0) = invalid/unknown; `SIGSET_V9_50_01_3344` (1) = the only tested version. |
| `OrlibVersionManifestEntry` | 249-280 | One entry in the known-versions manifest. Carries Tier 1 (FileVersion), Tier 2 (PE timestamp + file size), Tier 3 (SHA-256) indicators, plus `.text`/export sanity indicators and PE version-resource metadata. Compiled in as a `constexpr` array. |
| `VersionTier` (enum class) | 288-295 | Tier-match level reached during detection: `Unknown`, `Tier1Match`, `Tier2Match`, `Tier2Mismatch`, `Tier3Match`, `Tier3Mismatch`. |
| `VersionDescriptor` | 301-311 | Structured result of `detectVersion()`. Carries `known` flag, `tier`, `fileVersion`, `peTimestamp`, `fileSize`, `sha256`, `manifestEntry` pointer, `signatureSet`, and `warningMessage`. Never represents a fatal error. |
| `PatchSiteDescriptor` | 318-326 | Describes a resolved patch site: `name`, `patchRva`, `sigMatchRva`, `patchOffset`, `originalBytes`, `replacementBytes`, `containingFunction`. |
| `OrlibResolverError` | 339-358 | Exception type (derived from `std::runtime_error`) for unrecoverable resolution failures. Carries `target()` and `detail()` accessors for structured diagnostics. |

### 3.2 Typed function-pointer typedefs (lines 372-407)

The header declares typedefs for all 7 exports (all `__stdcall`) and all 8 internal functions (CALL_TARGETs, with their confirmed calling conventions). These match the typedefs already in `src/tsdev_lib.h` (lines 143-186) and the SEH-wrapper typedefs in `src/tsdev_lib.cpp` (lines 116-120) so the resolver's return values assign directly to `TsDevLib`'s member variables without adapter casts. Notable: `DeleteFromTree` (Target 6) and `FUN_101a8360` (Target 8) are stored as `void*` because MSVC has no `__thiscall` function-pointer type and the delete orchestrator is invoked via inline asm.

### 3.3 Construction / destruction (lines 437-476)

| Method | Lines | Purpose |
|---|---|---|
| `OrlibResolver(const wchar_t* dllPath)` | 446 | Production constructor: loads the real DLL via `LoadLibraryW`, acquires the `HMODULE`, parses PE headers. Throws `OrlibResolverError` on load/PE failure. |
| `OrlibResolver(const uint8_t* image, size_t size, uintptr_t base)` | 464 | Test constructor: operates on an in-memory PE image buffer without loading the DLL. No `HMODULE`; `VirtualProtect` is a no-op; exports resolved by parsing the PE export table from the buffer. This is the unit-test seam. |
| `~OrlibResolver()` | 471 | Destructor: does NOT call `FreeLibrary` in production mode (matching `TsDevLib`'s existing behavior — internal threads/state may reference the DLL). In test mode, nothing to release. |
| Copy/move | 473-476 | Not copyable (owns `HMODULE`/buffer); movable (transfers ownership). |

### 3.4 Version detection (lines 478-502)

| Method | Lines | Purpose |
|---|---|---|
| `VersionDescriptor detectVersion()` | 495 | Implements the 3-tier decision tree. **Never throws.** Unknown/rebuilt/byte-differs are warnings. |
| `const VersionDescriptor& version() const` | 502 | Accessor for the cached result of `detectVersion()` (called once at construction). |

### 3.5 Generic signature resolution (lines 504-542)

| Method | Lines | Purpose |
|---|---|---|
| `uintptr_t resolveBySignature(const std::string& name, const OrlibSignatureDef& signature)` | 534-535 | Generic resolution: scans `.text`, enforces uniqueness (exactly 1 match), returns the match RVA. Throws on no-match/multi-match. |
| `void* resolveAddressBySignature(const std::string& name, const OrlibSignatureDef& signature)` | 541-542 | Convenience wrapper: `baseAddress() + resolveBySignature(...)`. |

### 3.6 Specific internal-function resolvers — 8 CALL_TARGETs (lines 544-607)

Each method resolves its target by signature scan (via `resolveBySignature`) and returns a typed function pointer. All throw `OrlibResolverError` on no-match or multi-match.

| Method | Target # | RVA | Return type | Lines |
|---|---|---|---|---|
| `resolveStartCompilingProcess()` | 1 | 0x165DE0 | `StartCompilingProcessFn` (`__cdecl`) | 557 |
| `resolveEndCompilingProcess()` | 2 | 0x165ED0 | `EndCompilingProcessFn` (`__fastcall`) | 562 |
| `resolveCELCompilerCtor()` | 3 | 0x165A00 | `CELCompilerCtorFn` (`__fastcall`) | 567 |
| `resolveCELCompilerDtor()` | 4 | 0x165AC0 | `CELCompilerDtorFn` (`__fastcall`) | 572 |
| `resolveInitGlobalLookup()` | 5 | 0x1A9460 | `InitGlobalLookupFn` (`__cdecl`) | 577 |
| `resolveDeleteFromTree()` | 6 | 0x1E4030 | `void*` (invoke via inline asm) | 588 |
| `resolveGetWarehouse()` | 7 | 0x1A9650 | `GetWarehouseFn` (`__cdecl`) | 598 |
| `resolveDeleteOrchestrator()` | 8 | 0x1A8360 | `void*` (invoke via inline asm) | 607 |

### 3.7 Export resolution — 7 exports (lines 609-658)

Exports are resolved by `GetProcAddress` (production) or by parsing the PE export table from the in-memory buffer (test). These are **not** signature-scanned — the export table provides the address directly.

| Method | Export name | Return type | Lines |
|---|---|---|---|
| `resolveOrlibInit()` | `_OrlibInit@8` | `OrlibInitFn` | 619 |
| `resolveOrlibExit()` | `_OrlibExit@0` | `OrlibExitFn` | 623 |
| `resolveFindExistPos()` | `FindExistPos` (undecorated) | `FindExistPosFn` | 631 |
| `resolveCreateEntryEX()` | `_CreateEntryEX@24` | `CreateEntryEXFn` | 635 |
| `resolveSaveModuleNew()` | `_SaveModule_New@20` | `SaveModule_NewFn` | 639 |
| `resolveVerifyModuleNew()` | `_VerifyModule_New@20` | `VerifyModule_NewFn` | 643 |
| `resolveCloseWarehouse()` | `_CloseWarehouseForCurrentThreadAndPath@4` | `CloseWarehouseFn` | 647 |
| `resolveExport(const char* decoratedName)` | (generic) | `FARPROC` | 658 |

### 3.8 Patch-site resolution & validation — 4 PATCH_SITEs (lines 660-778)

| Method | Target # | Lines | Purpose |
|---|---|---|---|
| `resolvePatchSite(const std::string& name)` | 9-12 | 680 | Scans the containing function's signature, computes the patch-site address (`sigMatchRva + patchOffset`), returns a `PatchSiteDescriptor`. Does not validate original bytes yet. |
| `validatePatchSite(name, rva, expectedBytes, mask)` | — | 707-710 | Reads bytes at `baseAddress() + rva`, compares (with wildcard mask) against `expectedBytes`. Returns `bool`. Throws if address is outside `.text`. |
| `validatePatchSite(const PatchSiteDescriptor& desc)` | — | 722 | Convenience overload: uses the descriptor's original bytes + a wildcard mask derived from the signature definition. |
| `applyPatch(const std::string& name)` | 9-12 | 744 | Full patch operation: resolve → validate → idempotency check → `VirtualProtect` → `memcpy` → restore protection. In test mode, steps 4-6 are no-ops (validation only). Returns `bool`. |
| `resolvePersistencePatch()` | 9 | 755 | Resolve & validate Target 9 (FUN_10011230). Returns `PatchSiteDescriptor`. |
| `resolveThreadCheck1()` | 10 | 763 | Resolve & validate Target 10 (FUN_101d0950). |
| `resolveThreadCheck2()` | 11 | 770 | Resolve & validate Target 11 (FUN_101cf5b0). |
| `resolveThreadCheck3()` | 12 | 778 | Resolve & validate Target 12 (FUN_101d1fa0). |

### 3.9 Data-section RVA resolution — 3 data globals (lines 780-820)

Data-section RVAs cannot be matched by instruction-pattern signatures. The resolution strategy (`docs/SIGNATURE_CATALOG.md` §6) is: find the referencing code function by its code signature, extract the 4-byte absolute address operand from the disassembled instruction, and convert to an RVA (subtract image base).

| Method | Global | Referencing function | Lines |
|---|---|---|---|
| `resolveErrorArrayPtrRva()` | `DAT_1023cea0` (RVA 0x23CEA0) | StartCompilingProcess (Target 1) at instruction RVA 0x165DF9 | 804 |
| `resolveErrorCountRva()` | `DAT_1023cea4` (RVA 0x23CEA4) | StartCompilingProcess (Target 1) | 812 |
| `resolveErrorHinstRva()` | `DAT_1023aab8` (RVA 0x23AAB8) | Error-formatting code path | 820 |

### 3.10 Accessors (lines 822-843)

| Method | Lines | Purpose |
|---|---|---|
| `uintptr_t baseAddress() const` | 829 | The loaded DLL base (production) or simulated base (test). |
| `HMODULE moduleHandle() const` | 833 | Raw `HMODULE`, or `nullptr` in test mode. |
| `std::pair<uintptr_t, size_t> textSectionBounds() const` | 837 | `.text` section bounds (`{textRva, textRawSize}`). |
| `bool isTestMode() const` | 840 | Whether the resolver is in test mode (in-memory buffer). |
| `SignatureSetId signatureSet() const` | 843 | The selected signature set (from `detectVersion`). |

### 3.11 Protected virtual test seam (lines 845-885)

| Method | Lines | Purpose |
|---|---|---|
| `virtual std::vector<uintptr_t> scanTextSection(const OrlibSignatureDef& sig)` | 861 | Core scan primitive: iterates `.text` raw bytes, collects all match RVAs. The public `resolveBySignature()` calls this and enforces uniqueness. Overridable in test subclasses. |
| `virtual void parsePeHeaders()` | 868 | Extracts image base, `.text` bounds, export directory. Called once at construction. |
| `virtual std::string readFileVersion()` | 876 | Reads FileVersion from the PE version resource. |
| `virtual std::string computeSha256()` | 885 | Computes SHA-256 of the on-disk file (production) or in-memory buffer (test). |

### 3.12 Private state & helpers (lines 887-926)

The class holds: `m_testMode`, `m_module` (`HMODULE`), `m_base`, `m_image`/`m_imageSize` (test buffer), parsed PE metadata (`.text` bounds, export directory), cached `m_version` (`VersionDescriptor`), and `m_signatures` (the 12 `OrlibSignatureDef` entries loaded at construction). Private helpers: `findSignature(name)` (lookup by target name, throws if not found) and `resolveExportFromBuffer(decoratedName)` (test-mode export resolution by walking the PE export directory in the in-memory buffer).

---

## 4. Signature-Scan Algorithm

The scan is implemented by the protected virtual `scanTextSection()` (line 861) and enforced by the public `resolveBySignature()` (lines 534-535). The algorithm, as documented in the header (lines 513-528):

### Step-by-step

1. **Pattern representation.** Each `OrlibSignatureDef` carries parallel `bytes` and `mask` arrays of length `length`. A mask byte of `0xFF` means the corresponding byte is fixed (must match exactly); `0x00` means wildcard (`??`, matches any byte). The implementation (TVS-B.4) converts the `kSignatures[]` hex patterns from `b1_signatures.h` into this structured form at startup (or compile time), so no hex-string parsing occurs at scan time.

2. **Scan bounds.** The scan iterates over the `.text` section raw bytes, as parsed by `parsePeHeaders()`. For each offset `i` in `[0, textRawSize - sigLen]`, the matcher checks whether all non-wildcard bytes match: for each byte position `j`, if `mask[j] == 0xFF` then `textBytes[i + j]` must equal `bytes[j]`; if `mask[j] == 0x00`, the byte is skipped. This is the same wildcard-aware matching used by the TVS-B.1 verification probe (`src/sig_scan.cpp`, `docs/SIGNATURE_CATALOG.md` §8).

3. **Collect matches.** Every offset where all non-wildcard bytes match is collected. Offsets are converted to RVAs: `matchRva = textRva + offset`.

4. **Uniqueness enforcement.** `resolveBySignature()` requires **exactly 1 match**:
   - **0 matches (no-match):** throw `OrlibResolverError("signature no-match", name, detail)` where `detail` includes the signature name, the hex pattern, and a hex dump of the first and last 256 bytes of `.text` so the failure is diagnosable without a debugger (header lines 520-523).
   - **>1 matches (multi-match):** throw `OrlibResolverError("signature multi-match", name, detail)` where `detail` includes the signature name, the match count, and all match RVAs (header lines 524-527). A multi-match means the signature is not unique and must be extended — as was done for Target 12 during TVS-B.1 (extended from 41 to 57 bytes to disambiguate a spurious match at RVA `0x001CFCE0`).

5. **Return.** The single match RVA is returned. `resolveAddressBySignature()` adds `baseAddress()` to return an absolute address.

### The protected virtual seam

`scanTextSection()` is `protected virtual` (line 861) specifically to enable testing. A test subclass can override it to:
- Inject fake scan behavior (e.g., return a predetermined RVA without scanning).
- Fuzz the matcher (e.g., mutate signature bytes and observe match counts).
- Short-circuit specific targets.

The production path feeds the loaded DLL's `.text` section (from `parsePeHeaders()`); the test path feeds the `.text` section parsed from the in-memory buffer (the test constructor, line 464). Both paths use the same `resolveBySignature()` uniqueness enforcement, so tests exercise the real error-handling logic.

---

## 5. Version Detection & Manifest

### The 3-tier method

`detectVersion()` (line 495) implements the tiered multi-factor decision tree from `docs/VERSION_DETECTION.md` §4.2:

| Tier | Indicator(s) | Purpose | Cost | Action on match | Action on mismatch |
|---|---|---|---|---|---|
| **Tier 1** | FileVersion string | Fast lookup in the known-versions manifest | Low | Proceed to Tier 2 | Version is **unknown** → warning, proceed to signature resolution |
| **Tier 2** | PE timestamp + file size | Catch rebuilds with same FileVersion | Low | Proceed to Tier 3 | Version is **known but rebuilt** → warning, proceed |
| **Tier 3** | SHA-256 | Confirm exact byte-identity with manifest entry | High | Report **"known & verified"** | Report **"known but byte-differs"** → warning, proceed |

The tiering is ordered by increasing cost and specificity: Tier 1 is the cheap gate (no file hashing); Tier 2 adds a cheap cross-check (4-byte timestamp + file size); Tier 3 is the definitive check, computed only when Tiers 1 and 2 already match, so the expensive hash is never computed for unknown versions or obvious mismatches.

### The VersionDescriptor return

`detectVersion()` returns a `VersionDescriptor` (lines 301-311) carrying:
- `known` — whether FileVersion was found in the manifest.
- `tier` — the highest `VersionTier` reached (`Unknown`, `Tier1Match`, `Tier2Match`, `Tier2Mismatch`, `Tier3Match`, `Tier3Mismatch`).
- `fileVersion`, `peTimestamp`, `fileSize`, `sha256` — the indicators read from the DLL.
- `manifestEntry` — pointer to the matched `OrlibVersionManifestEntry`, or `nullptr` if unknown.
- `signatureSet` — the selected `SignatureSetId` (`SIGSET_NONE` if unknown).
- `warningMessage` — human-readable warning (empty if known & verified).

**Version detection never throws.** Unknown/rebuilt/byte-differs states are warnings returned in the descriptor, not exceptions. The caller (`TsDevLib` constructor in TVS-B.5) prints the warning to stderr and proceeds.

### The OrlibVersionManifestEntry struct

`OrlibVersionManifestEntry` (lines 249-280) mirrors the struct defined in `docs/VERSION_DETECTION.md` §6.2. It is compiled in as a `constexpr` array (`kOrlibVersionManifest`) — no runtime JSON parsing. One entry per known-tested DLL version. Currently one entry: `9.50.01.3344` (PE timestamp `0x5C01735E`, file size 4,924,400, SHA-256 `318DB24A...EC18`, `SIGSET_V9_50_01_3344`).

The header re-declares this struct (rather than hard-including a `tmp/` artifact) so it is self-contained. The implementation (TVS-B.4) bridges the two definitions.

### Resolution order: signature scan first, manifest fallback

The header's design rationale (lines 77-98) explicitly chose **signature scan first, manifest fallback** — not the other way around. The rationale:

1. **Signatures are self-validating.** A unique match proves the function is there, regardless of what the manifest claims. A manifest RVA is only a claim; it cannot prove the bytes are correct.

2. **The manifest's primary value is version detection and patch-site original-byte lookup**, not function location. For patch-site validation, the manifest supplies the expected original bytes; the signature scan supplies the address.

3. **Unknown versions can still succeed.** If the version is unknown (not in the manifest), signatures may still match (a minor rebuild often preserves prologue patterns). Scanning first means an unknown version can succeed with only a warning. If a manifest hit were required first, unknown versions would be blocked even when the code is functionally identical.

4. **Manifest RVAs as last-resort fallback.** The manifest can supply RVAs as a last-resort fallback for targets whose signatures fail to match (e.g., the function was rewritten but the version is known). In that case the manifest entry's recorded RVAs are used with a warning, accepting the risk that the RVA may be wrong. This fallback is opt-in and loudly warned.

This order separates two concerns: "is this a known/tested version?" (version detection — warning) and "do the signatures match?" (resolution — hard error). An untested version might still work; a failed signature scan definitely won't.

---

## 6. Patch-Site Validation

### The three operations

Patch-site handling involves three operations, declared at lines 680-744:

1. **`resolvePatchSite(name)`** (line 680): Scans the containing function's signature, computes the patch-site address (`sigMatchRva + patchOffset`), and returns a `PatchSiteDescriptor` with the patch RVA, original bytes, and replacement bytes. Does **not** validate original bytes yet.

2. **`validatePatchSite(...)`** (lines 707-722): Reads `expectedBytes.size()` bytes at `baseAddress() + rva` and compares them (with wildcard mask support) against `expectedBytes`. This is the pre-patch safety check: if the bytes don't match, the patch must **not** be applied (the DLL is a different build than expected). Returns `bool`.

3. **`applyPatch(name)`** (line 744): The full patch operation:
   - Resolve the patch site (`resolvePatchSite`).
   - Validate original bytes (`validatePatchSite`) — throw on mismatch.
   - Check idempotency: if replacement bytes are already present, skip (return `true`).
   - `VirtualProtect` to `PAGE_EXECUTE_READWRITE`.
   - `memcpy` replacement bytes.
   - Restore original protection.
   - In test mode (in-memory buffer), steps 4-6 are no-ops (buffer is read-only); only validation is performed.

### The PatchSiteDescriptor

`PatchSiteDescriptor` (lines 318-326) carries:
- `name` — patch-site name (e.g., `"FUN_10011230_persistence_patch"`).
- `patchRva` — RVA where bytes are written.
- `sigMatchRva` — RVA where the signature matched (containing function entry).
- `patchOffset` — offset from sig match to patch site.
- `originalBytes` — expected original bytes at the patch site (for validation).
- `replacementBytes` — replacement bytes to write.
- `containingFunction` — name of the containing function (diagnostic).

### Original-bytes validation with wildcard mask

The validation pattern mirrors the existing `TsDevLib::validatePersistencePatchBytes` logic (`src/tsdev_lib.cpp` lines 757-772). For the **persistence patch** (Target 9, FUN_10011230), the original 8 bytes are `55 8B EC 6A FF 68 CD D3`. Bytes 6-7 (`CD D3`) are the low 2 bytes of the SEH handler address (`0x101ED3CD`), which is image-base-relative and subject to ASLR relocation. These bytes are wildcarded in validation (mask `0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0x00 0x00`, `src/tsdev_lib.cpp` lines 72-77). The `validatePatchSite` overload taking a `PatchSiteDescriptor` (line 722) derives this wildcard mask from the signature definition automatically.

For the **INT3 patches** (Targets 10-12), no wildcards are needed: the original byte is a single `0xCC` (INT3), and the replacement is `0x90` (NOP). The existing production code (`src/tsdev_lib.cpp` lines 816-864, `applyThreadCheckPatches`) already verifies the byte is either `0xCC` (unpatched) or `0x90` (already patched) before writing — this idempotency check is preserved in `applyPatch`.

### Reference to TVS-B.1 patch-byte verification

The patch-site original bytes and replacement bytes for all 4 patch sites are documented in `docs/SIGNATURE_CATALOG.md` §3 (Patch-site metadata table) and encoded in `tmp/re_artifacts/b1_signatures.h` (the `originalBytes`, `replacementBytes`, and `patchOffset` fields of each patch-site `SignatureDef`). The TVS-B.1 scan run verified that the byte at each patch offset matches the expected original byte (§8, result table: "patch byte 0x55 OK" for Target 9, "patch byte 0xCC OK" for Targets 10-12).

---

## 7. Coverage Table

This table maps every one of the 12 RVA targets (8 CALL_TARGETs + 4 PATCH_SITEs) and all 7 exports to its resolution method(s) in the header. It reconciles against `docs/SIGNATURE_CATALOG.md` (authoritative) and `tmp/re_artifacts/b1_signatures.h`.

### 7.1 Internal functions — 8 CALL_TARGETs (signature scan)

| # | Name | Target RVA | Header method | Resolution method | Signature source | Uniqueness (TVS-B.1) |
|---|---|---|---|---|---|---|
| 1 | StartCompilingProcess | 0x165DE0 | `resolveStartCompilingProcess()` (line 557) | Signature scan | `b1_signatures.h` Target 1 (64 bytes) | 1/1 PASS |
| 2 | EndCompilingProcess | 0x165ED0 | `resolveEndCompilingProcess()` (line 562) | Signature scan | `b1_signatures.h` Target 2 (55 bytes) | 1/1 PASS |
| 3 | CELCompiler_ctor | 0x165A00 | `resolveCELCompilerCtor()` (line 567) | Signature scan | `b1_signatures.h` Target 3 (64 bytes) | 1/1 PASS |
| 4 | CELCompiler_dtor | 0x165AC0 | `resolveCELCompilerDtor()` (line 572) | Signature scan | `b1_signatures.h` Target 4 (38 bytes) | 1/1 PASS |
| 5 | InitGlobalLookup | 0x1A9460 | `resolveInitGlobalLookup()` (line 577) | Signature scan | `b1_signatures.h` Target 5 (66 bytes) | 1/1 PASS |
| 6 | DeleteFromTree | 0x1E4030 | `resolveDeleteFromTree()` (line 588) | Signature scan (returns `void*`) | `b1_signatures.h` Target 6 (66 bytes) | 1/1 PASS |
| 7 | GetWarehouse | 0x1A9650 | `resolveGetWarehouse()` (line 598) | Signature scan (resolved-but-unused) | `b1_signatures.h` Target 7 (45 bytes) | 1/1 PASS |
| 8 | FUN_101a8360_delete_orchestrator | 0x1A8360 | `resolveDeleteOrchestrator()` (line 607) | Signature scan (returns `void*`) | `b1_signatures.h` Target 8 (64 bytes) | 1/1 PASS |

### 7.2 Patch sites — 4 PATCH_SITEs (signature scan + validation)

| # | Name | Patch-site RVA | Sig match RVA | Patch offset | Header method | Original bytes | Replacement bytes |
|---|---|---|---|---|---|---|---|
| 9 | FUN_10011230_persistence_patch | 0x11230 | 0x11230 | 0x00 | `resolvePersistencePatch()` (line 755) / `applyPatch()` (line 744) | `55 8B EC 6A FF 68 CD D3` (bytes 6-7 wildcarded) | `B8 01 00 00 00 C2 0C 00` |
| 10 | FUN_101d0950_thread_check_INT3 | 0x1D0993 | 0x1D0950 | 0x43 | `resolveThreadCheck1()` (line 763) / `applyPatch()` | `CC` | `90` |
| 11 | FUN_101cf5b0_thread_check_INT3 | 0x1CF5D4 | 0x1CF5B0 | 0x24 | `resolveThreadCheck2()` (line 770) / `applyPatch()` | `CC` | `90` |
| 12 | FUN_101d1fa0_thread_check_INT3 | 0x1D1FC8 | 0x1D1FA0 | 0x28 | `resolveThreadCheck3()` (line 778) / `applyPatch()` | `CC` | `90` |

All 4 patch sites are resolved via `resolvePatchSite()` (line 680), which scans the **containing function's** signature and computes the patch-site address as `sigMatchRva + patchOffset`. For Target 9, the patch is at the function entry (offset 0); for Targets 10-12, the signature matches the containing function and the patch byte is at a nonzero offset within it.

### 7.3 Exports — 7 exports (GetProcAddress / PE export table)

| # | Export name | Decorated? | Header method | Resolution method |
|---|---|---|---|---|
| 1 | `_OrlibInit@8` | Yes (stdcall) | `resolveOrlibInit()` (line 619) | `GetProcAddress` (production) / PE export table parse (test) |
| 2 | `_OrlibExit@0` | Yes (stdcall) | `resolveOrlibExit()` (line 623) | Same |
| 3 | `FindExistPos` | **No — undecorated** | `resolveFindExistPos()` (line 631) | Same |
| 4 | `_CreateEntryEX@24` | Yes (stdcall) | `resolveCreateEntryEX()` (line 635) | Same |
| 5 | `_SaveModule_New@20` | Yes (stdcall) | `resolveSaveModuleNew()` (line 639) | Same |
| 6 | `_VerifyModule_New@20` | Yes (stdcall) | `resolveVerifyModuleNew()` (line 643) | Same |
| 7 | `_CloseWarehouseForCurrentThreadAndPath@4` | Yes (stdcall) | `resolveCloseWarehouse()` (line 647) | Same |

All 7 exports are also accessible via the generic `resolveExport(const char* decoratedName)` (line 658), which returns a raw `FARPROC`.

### 7.4 Data-section globals — 3 RVAs (code-signature-derived)

| # | Global | RVA | Header method | Resolution strategy |
|---|---|---|---|---|
| 13 | `DAT_1023cea0` (error array ptr) | 0x23CEA0 | `resolveErrorArrayPtrRva()` (line 804) | Find StartCompilingProcess (Target 1) by signature; extract 4-byte absolute address from `MOV ECX, [imm32]` at instruction RVA 0x165DF9; subtract image base |
| 14 | `DAT_1023cea4` (error count) | 0x23CEA4 | `resolveErrorCountRva()` (line 812) | Same referencing function; extract from `MOV [imm32], EAX` |
| 15 | `DAT_1023aab8` (string-table HINSTANCE) | 0x23AAB8 | `resolveErrorHinstRva()` (line 820) | Find referencing error-formatting code by signature; extract data reference |

These 3 data globals are currently hardcoded in `src/tsdev_lib.cpp` (lines 148-150: `kErrArrayPtrRva`, `kErrCountRva`, `kErrHinstRva`). The resolver replaces them with code-signature-derived resolution, tying data-offset resolution to code-signature resolution so both are robust to DLL version changes.

### 7.5 Summary counts

| Category | Count | Resolution method |
|---|---|---|
| CALL_TARGETs (internal functions) | 8 | Signature scan |
| PATCH_SITEs | 4 | Signature scan (containing function) + original-byte validation |
| Exports | 7 | `GetProcAddress` / PE export table parse |
| Data-section globals | 3 | Code-signature-derived (extract data ref from referencing instruction) |
| **Total RVA targets** | **12** (8 + 4) | — |

### 7.6 The "8 exports" discrepancy

The TVS-B.3 handoff's adversarial-review line mentioned "8 exports", but `docs/SIGNATURE_CATALOG.md` §5 (authoritative, confirmed via `dumpbin /exports`) lists exactly **7** export names used in production code. The header's design rationale (lines 172-183) addresses this discrepancy explicitly:

> The "8" likely conflates the 8 CALL_TARGET internal functions with the 7 exports, or counts `_GetCompilingErrors@4` (which was **removed** in TVS-A.4 — the CLI now reads the error array directly via the 3 data-section globals, not via the `_GetCompilingErrors@4` export). No 8th export is resolved here.

This document follows `SIGNATURE_CATALOG.md` (7 exports). The removed `_GetCompilingErrors@4` is documented in `AGENTS.md` (TVS-A.4 section) and replaced by direct global array access (`readErrorsFromArray` in `src/tsdev_lib.cpp` lines 158-207), which the resolver supports via the 3 data-section RVA resolution methods (§7.4).

---

## 8. Error Handling

### Exception strategy

The resolver uses **exceptions**, not return codes, for all **unrecoverable** resolution failures. The custom exception type `OrlibResolverError` (lines 339-358) is derived from `std::runtime_error` so existing catch handlers in `TsDevLib` (which catch `std::runtime_error`) continue to work. It carries structured diagnostic context:

- `what()` — composed from `whatArg + ": target='" + target + "' " + detail`.
- `target()` — the target name (e.g., `"StartCompilingProcess"`) or export name.
- `detail()` — full diagnostic (hex dump, match RVAs, expected vs. actual bytes, etc.).

### When exceptions are used

| Failure | Exception | Detail carried |
|---|---|---|
| Signature no-match (scan count = 0) | `OrlibResolverError("signature no-match", ...)` | Signature name, hex pattern, hex dump of first/last 256 bytes of `.text` |
| Signature multi-match (scan count > 1) | `OrlibResolverError("signature multi-match", ...)` | Signature name, match count, all match RVAs |
| Export resolution failure (`GetProcAddress` returns NULL) | `OrlibResolverError(...)` | Decorated export name, `GetLastError()` |
| Patch-site validation failure (bytes don't match) | `OrlibResolverError(...)` | Patch-site name, RVA, expected vs. actual bytes |
| `LoadLibraryW` failure or invalid PE headers | `OrlibResolverError(...)` | Load error / PE validation detail |
| `VirtualProtect` failure during `applyPatch` | `OrlibResolverError(...)` | Patch-site name, RVA, `GetLastError()` |

### When return values are used

Recoverable operations use `bool` return values, not exceptions:
- `validatePatchSite(...)` returns `bool` (true = bytes match, false = mismatch). The caller decides whether to proceed.
- `applyPatch(name)` returns `bool` (true = patch applied or already applied).

### Version detection never throws

Version detection (`detectVersion()`) **never** throws. Unknown / rebuilt / byte-differs states are returned as warnings in the `VersionDescriptor`, not as exceptions. This is the key separation of concerns (header lines 68-75, `docs/VERSION_DETECTION.md` §4.2): "untested version" is a warning; "signatures don't match" is a hard error. An untested version might still work, but a failed signature scan definitely won't.

### The "fail loudly" principle

This strategy aligns with the project's acceptance criteria: the three core functions (insert, delete, modify) must work **reliably and consistently**. A silent proceed into the wrong code address would produce unreliable behavior (crashes, state corruption) — the opposite of reliable. By throwing with structured diagnostics on resolution failure, the resolver ensures the CLI fails loudly with a clear message rather than silently calling into the wrong code.

---

## 9. Unit-Testability Without a Live TS Install

### The test seam

The resolver is designed to be fully unit-testable without a TradeStation installation. Two mechanisms enable this:

1. **In-memory constructor** (line 464): `OrlibResolver(const uint8_t* image, size_t size, uintptr_t base)` operates on an in-memory PE image buffer **without** loading the DLL. No `HMODULE` is acquired; `VirtualProtect` is a no-op; export resolution is simulated by parsing the PE export table from the buffer. Signature scanning operates on the `.text` section parsed from the buffer.

2. **Protected virtual methods** (lines 861-885): `scanTextSection()`, `parsePeHeaders()`, `readFileVersion()`, and `computeSha256()` are all `protected virtual`, so a test subclass can override them to inject custom behavior:
   - `scanTextSection()` — override to inject fake scan results or fuzz the matcher.
   - `parsePeHeaders()` — override to inject synthetic PE metadata.
   - `readFileVersion()` — override to return a predetermined version string.
   - `computeSha256()` — override to return a predetermined hash.

### Concrete test scenario

A test (`test_orlib_resolver`, planned for TVS-B.6) can run on any machine without a TradeStation install:

1. **Load a copied DLL into memory.** Read `tests/fixtures/orlib20_baseline.dll` (the committed baseline copy, byte-identical to the install original) into a `std::vector<uint8_t>` buffer.

2. **Construct the resolver in test mode.** `OrlibResolver resolver(buffer.data(), buffer.size(), 0x10000000)` — no `LoadLibraryW`, no `HMODULE`.

3. **Detect version.** `auto vd = resolver.detectVersion()` — `readFileVersion()` parses the version resource from the buffer; `parsePeHeaders()` extracts the PE timestamp and `.text` bounds; `computeSha256()` hashes the buffer. Assert `vd.known == true`, `vd.tier == VersionTier::Tier3Match`, `vd.fileVersion == "9.50.01.3344"`.

4. **Resolve all 12 signatures.** Call each `resolve*` method (8 CALL_TARGETs + 4 patch-site `resolvePatchSite` calls). Assert each returns the expected RVA (matching `b1_signatures.h` `rva` fields). Assert no exceptions are thrown.

5. **Assert uniqueness.** Each `resolveBySignature` call enforces exactly 1 match. If any signature matched 0 or >1 times, an `OrlibResolverError` would be thrown — the test catches this and fails with the diagnostic.

6. **Validate patch sites.** Call `validatePatchSite` for each of the 4 patch sites. Assert `true` (the baseline DLL is unpatched, so original bytes are present).

7. **Resolve all 7 exports.** Call each `resolve*` export method. Assert each returns a non-null pointer (resolved from the PE export table in the buffer).

8. **Resolve 3 data-section globals.** Call `resolveErrorArrayPtrRva()`, `resolveErrorCountRva()`, `resolveErrorHinstRva()`. Assert each returns the expected RVA (0x23CEA0, 0x23CEA4, 0x23AAB8).

This scenario exercises the real scan, validation, and export-resolution logic against a real PE image, without requiring a live TS install or loading the DLL into the process.

---

## 10. Integration with TsDevLib

### Before: hardcoded RVAs (current `src/tsdev_lib.cpp`)

The current `TsDevLib` constructor (`src/tsdev_lib.cpp` lines 319-408) hardcodes everything:

```cpp
// Line 328: load the DLL
m_orlib = LoadLibraryW(L"orlib20.dll");
m_orlibBase = reinterpret_cast<DWORD_PTR>(m_orlib);

// Lines 367-377: resolve 7 exports by decorated name
m_pOrlibInit      = resolveOrThrow<OrlibInit_t>("_OrlibInit@8");
m_pOrlibExit      = resolveOrThrow<OrlibExit_t>("_OrlibExit@0");
m_pFindExistPos   = resolveOrThrow<FindExistPos_t>("FindExistPos");
m_pCreateEntry    = resolveOrThrow<CreateEntryEX_t>("_CreateEntryEX@24");
m_pSaveModuleNew  = resolveOrThrow<SaveModule_New_t>("_SaveModule_New@20");
m_pVerifyModule   = resolveOrThrow<VerifyModule_New_t>("_VerifyModule_New@20");
m_pCloseWarehouse = resolveOrThrow<CloseWarehouse_t>(
    "_CloseWarehouseForCurrentThreadAndPath@4");

// Lines 380-395: resolve 8 internal functions by HARDCODED RVA
m_pStartCompiling = reinterpret_cast<StartCompilingProcess_t>(
    m_orlibBase + kRvaStartCompiling);       // 0x165de0
m_pEndCompiling   = reinterpret_cast<EndCompilingProcess_t>(
    m_orlibBase + kRvaEndCompiling);         // 0x165ed0
m_pCELCompilerCtor = reinterpret_cast<CELCompilerCtor_t>(
    m_orlibBase + kRvaCELCompilerCtor);      // 0x165a00
m_pCELCompilerDtor = reinterpret_cast<CELCompilerDtor_t>(
    m_orlibBase + kRvaCELCompilerDtor);      // 0x165ac0
m_pInitGlobalLookup = reinterpret_cast<InitGlobalLookup_t>(
    m_orlibBase + kRvaInitGlobalLookup);     // 0x1a9460
m_pDeleteFromTree = reinterpret_cast<void*>(
    m_orlibBase + kRvaDeleteFromTree);       // 0x1e4030
m_pGetWarehouse = reinterpret_cast<GetWarehouse_t>(
    m_orlibBase + kRvaGetWarehouse);         // 0x1a9650

// Line 608-609: delete orchestrator by hardcoded RVA
void* deleteOrchestrator = reinterpret_cast<void*>(
    m_orlibBase + kRvaDeleteOrchestrator);   // 0x1a8360

// Lines 774-864: patches by hardcoded RVA
//   kRvaPersistencePatch = 0x11230
//   kRvaThreadCheck1 = 0x1d0993
//   kRvaThreadCheck2 = 0x1cf5d4
//   kRvaThreadCheck3 = 0x1d1fc8

// Lines 148-150: data-section globals by hardcoded RVA
static constexpr uintptr_t kErrArrayPtrRva  = 0x23cea0;
static constexpr uintptr_t kErrCountRva     = 0x23cea4;
static constexpr uintptr_t kErrHinstRva     = 0x23aab8;
```

The hardcoded RVA constants are defined at lines 50-95 (`kRvaStartCompiling`, `kRvaEndCompiling`, `kRvaCELCompilerCtor`, `kRvaCELCompilerDtor`, `kRvaInitGlobalLookup`, `kRvaDeleteFromTree`, `kRvaGetWarehouse`, `kRvaPersistencePatch`, `kRvaThreadCheck1/2/3`, `kRvaDeleteOrchestrator`) and lines 148-150 (`kErrArrayPtrRva`, `kErrCountRva`, `kErrHinstRva`).

### After: OrlibResolver (TVS-B.5)

The integration (TVS-B.5) replaces the constructor's hardcoded RVA arithmetic and `GetProcAddress` calls with `OrlibResolver` calls, as sketched in the header's design rationale (lines 155-170):

```cpp
// Construct the resolver (loads DLL, detects version, parses PE)
OrlibResolver resolver(L"orlib20.dll");

// Version detection — warning only, never throws
VersionDescriptor vd = resolver.version();  // cached from construction
if (!vd.known) {
    std::fprintf(stderr, "tsdev: WARNING — %s\n", vd.warningMessage.c_str());
}

// Resolve 7 exports (typed function pointers)
m_pOrlibInit      = resolver.resolveOrlibInit();
m_pOrlibExit      = resolver.resolveOrlibExit();
m_pFindExistPos   = resolver.resolveFindExistPos();
m_pCreateEntry    = resolver.resolveCreateEntryEX();
m_pSaveModuleNew  = resolver.resolveSaveModuleNew();
m_pVerifyModule   = resolver.resolveVerifyModuleNew();
m_pCloseWarehouse = resolver.resolveCloseWarehouse();

// Resolve 7 internal functions in the constructor (signature scan, typed
// function pointers). The 8th, the delete orchestrator, is resolved locally
// in deleteEntry(), not stored as a constructor member.
m_pStartCompiling   = resolver.resolveStartCompilingProcess();
m_pEndCompiling     = resolver.resolveEndCompilingProcess();
m_pCELCompilerCtor  = resolver.resolveCELCompilerCtor();
m_pCELCompilerDtor  = resolver.resolveCELCompilerDtor();
m_pInitGlobalLookup = resolver.resolveInitGlobalLookup();
m_pDeleteFromTree   = resolver.resolveDeleteFromTree();
m_pGetWarehouse     = resolver.resolveGetWarehouse();
// In deleteEntry(): void* del = resolver.resolveDeleteOrchestrator();
// (resolved locally per-call, not stored as a TsDevLib member)

// Apply 4 patches (resolve + validate + write)
resolver.applyPatch("FUN_10011230_persistence_patch");
resolver.applyPatch("FUN_101d0950_thread_check_INT3");
resolver.applyPatch("FUN_101cf5b0_thread_check_INT3");
resolver.applyPatch("FUN_101d1fa0_thread_check_INT3");

// Data-section globals (code-signature-derived)
uintptr_t errArrayRva = resolver.resolveErrorArrayPtrRva();
uintptr_t errCountRva = resolver.resolveErrorCountRva();
uintptr_t errHinstRva = resolver.resolveErrorHinstRva();
```

The resolver owns the `HMODULE` and base address; `TsDevLib` holds a resolver instance (or pointer) and queries it.

### Public API unchanged

The public `TsDevLib` API (`src/tsdev_lib.h` lines 49-133) is **unchanged** by this integration:

- `findExistPos`, `createEntry`, `saveSource`, `verifyModule`, `closeWarehouse`, `deleteEntry`, `modifyEntry`, `ellFileExists`, `getEllPath`, `getXmlPath`, `validatePersistencePatchBytes` — all signatures and behaviors preserved.
- The private member variables (`m_pOrlibInit`, `m_pStartCompiling`, etc.) remain the same types; they are simply populated by the resolver instead of by hardcoded RVA arithmetic.
- The SEH-protected call wrappers (`callCtor`, `callStartCompiling`, `callVerify`, `callEndCompiling`, `callDtor`, `callDeleteFromTree`, `callInitGlobalLookup`, `callGetWarehouse`, `callDeleteOrchestrator` in `src/tsdev_lib.cpp` lines 116-298) remain unchanged — they receive the same function pointers, just resolved differently.

This is purely an internal refactor: the same functions are resolved and called the same way; only the resolution mechanism changes from hardcoded RVA to signature scan + manifest fallback.

---

## 11. Out of Scope / Deferred to TVS-B.4

This TVS (B.3) produces **design documentation only**. The following are explicitly **not** in scope:

| Item | Deferred to |
|---|---|
| `.cpp` implementation of `OrlibResolver` (all method bodies) | TVS-B.4 |
| Actual integration into `src/tsdev_lib.cpp` (replacing hardcoded RVAs with resolver calls) | TVS-B.5 |
| Unit tests (`test_orlib_resolver.cpp`) | TVS-B.6 |
| Build changes (CMakeLists.txt additions for the new `.cpp`) | TVS-B.4 |
| The `constexpr` manifest array definition (`kOrlibVersionManifest[]`) as a separate header or within the resolver `.cpp` | TVS-B.4 |
| SHA-256 implementation choice (`BCryptCreateHash` vs. self-contained) | TVS-B.4 |
| Conversion of `kSignatures[]` hex patterns to `OrlibSignatureDef` byte/mask arrays | TVS-B.4 |
| Data-section RVA extraction (instruction disassembly to extract the 4-byte absolute address operand) | TVS-B.4 |

### The header is a design artifact with declared-only methods

As stated in the header (lines 16-19): "This is a DESIGN ARTIFACT ONLY (header interface). No `.cpp` implementation file is produced by TVS-B.3; implementation is TVS-B.4, TsDevLib integration is TVS-B.5. Method bodies are declared-only (no inline definitions) — the header compiles standalone; linking requires the future `.cpp`."

The header contains no inline method definitions. Every method is declared but not defined. The header compiles standalone (it includes only standard headers and `<windows.h>`); linking against it requires the future `.cpp` implementation from TVS-B.4.
