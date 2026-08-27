# Handoff B — Robustness & Future-Proofing Architecture

**Priority:** (b) Robustness, including an architecture that allows easy adaptation to future TS/TSDev updates.

**Date:** 2026-08-16

---

> **WORKSTREAM E NOTICE (2026-08-17):** Workstream E (`docs/ARCHITECTURE_REDESIGN.md`) is a new foundational workstream that should be executed BEFORE B.6/B.7. It fixes the build dependency on `tmp/re_artifacts/b1_signatures.h`, migrates signature data to version-controlled JSON with build-time codegen, adds `sigtool` for signature verification, archives 26 experimental probes, and eliminates RVA duplication.
>
> **Impact on B.7:** Items (c) structural constants externalization and (d) file-naming pattern are SUPERSEDED by E's JSON version manifest. B.7's remaining scope is (a) TS install path discovery + `--ts-path` CLI override and (b) derive MyWork\Bin paths from install root. — **B.7 (items a+b) COMPLETE & APPROVED (2026-08-18).**
>
> **Impact on B.6:** E's `sigtool verify` command complements the regression suite. B.6 should include `sigtool verify` as part of the regression suite.

---

## 1. Priority Statement

The tsdev_automation CLI currently works correctly against the installed TradeStation 9.5 `orlib20.dll`, but it is **fragile against version updates**. Every internal function is resolved by a hardcoded RVA offset, every binary patch expects exact byte patterns, and there is no version detection or signature-based function discovery. If TradeStation ships a new `orlib20.dll` — even a minor rebuild — the RVAs will shift, the patch targets will move, and the CLI will either crash (calling into the wrong code) or silently fail (patching the wrong bytes). This workstream makes the CLI **degrade gracefully or adapt**: functions are found by stable byte-pattern signatures (or export names where available), version mismatches are detected and reported clearly, patches are validated before application, and a regression suite catches breakage automatically.

---

## 2. Current State — Complete Inventory of Hardcoded Values

### 2.1 Hardcoded RVAs (internal functions, resolved by base + offset)

All are in `src/tsdev_lib.cpp` (anonymous namespace, lines 50–85) and resolved in the constructor (lines 319–335).

| Symbol | RVA | File:Line (definition) | File:Line (usage) | Calling Convention | Purpose |
|--------|-----|------------------------|-------------------|--------------------|---------|
| `kRvaStartCompiling` | `0x165de0` | `tsdev_lib.cpp:51` | `tsdev_lib.cpp:320–321` | `__cdecl`, 0 args | Resets error array, allocs compiler state |
| `kRvaEndCompiling` | `0x165ed0` | `tsdev_lib.cpp:52` | `tsdev_lib.cpp:322–323` | `__fastcall`, 1 arg | Tears down compiler state |
| `kRvaCELCompilerCtor` | `0x165a00` | `tsdev_lib.cpp:53` | `tsdev_lib.cpp:324–325` | `__fastcall` (ECX=this) | CELCompiler constructor |
| `kRvaCELCompilerDtor` | `0x165ac0` | `tsdev_lib.cpp:54` | `tsdev_lib.cpp:326–327` | `__fastcall` (ECX=this) | CELCompiler destructor |
| `kRvaInitGlobalLookup` | `0x1a9460` | `tsdev_lib.cpp:56` | `tsdev_lib.cpp:330–331` | `__cdecl`, 0 args | Returns global lookup object (DELETE flow) |
| `kRvaDeleteFromTree` | `0x1e4030` | `tsdev_lib.cpp:57` | `tsdev_lib.cpp:332–333` | `__thiscall` (ECX=globalLookup) | BST node removal (DELETE flow) |
| `kRvaGetWarehouse` | `0x1a9650` | `tsdev_lib.cpp:58` | `tsdev_lib.cpp:334–335` | `__cdecl`, 1 arg | Returns per-thread warehouse object |
| `kRvaDeleteOrchestrator` | `0x1a8360` | `tsdev_lib.cpp:85` | `tsdev_lib.cpp:546–547` | `__fastcall` (ECX=whType, EDX=progId) | Full delete orchestrator (UI delete path) |

### 2.2 Hardcoded RVAs (binary patches)

| Symbol | RVA | File:Line (definition) | File:Line (usage) | Patch Type |
|--------|-----|------------------------|-------------------|------------|
| `kRvaPersistencePatch` | `0x11230` | `tsdev_lib.cpp:63` | `tsdev_lib.cpp:696–697` | 8-byte code patch (SymWriter bypass) |
| `kRvaThreadCheck1` | `0x1d0993` | `tsdev_lib.cpp:76` | `tsdev_lib.cpp:714–718` | 1-byte INT3→NOP (FUN_101d0950, cmd 0x0F) |
| `kRvaThreadCheck2` | `0x1cf5d4` | `tsdev_lib.cpp:77` | `tsdev_lib.cpp:714–718` | 1-byte INT3→NOP (FUN_101cf5b0, general dispatch) |
| `kRvaThreadCheck3` | `0x1d1fc8` | `tsdev_lib.cpp:78` | `tsdev_lib.cpp:714–718` | 1-byte INT3→NOP (FUN_101d1fa0, cmd 0x17) |

### 2.3 Hardcoded byte patterns (patch payloads and validation)

| Pattern | Bytes | File:Line | Context |
|---------|-------|-----------|---------|
| Persistence patch payload | `B8 01 00 00 00 C2 0C 00` | `tsdev_lib.cpp:64–67` | `MOV EAX,1; RET 0x0C` — written to RVA 0x11230 |
| Thread-check expected original byte | `0xCC` (INT3) | `tsdev_lib.cpp:741` | Validated before NOP'ing; also accepts `0x90` (idempotent) |
| Thread-check patch byte | `0x90` (NOP) | `tsdev_lib.cpp:751` | Written over the INT3 |

**Note:** The persistence patch currently does **NOT** validate the original bytes before overwriting (`applyPersistencePatch()`, lines 695–709). It blindly writes the 8-byte payload. The thread-check patches **DO** validate (lines 729–750), checking for `0xCC` or `0x90`.

### 2.4 Exported functions resolved by decorated name (not RVA — already robust)

These are resolved via `GetProcAddress` in the constructor (lines 306–317). They are already version-robust as long as the export names don't change.

| Export Name | Decorated? | File:Line |
|-------------|-----------|-----------|
| `_OrlibInit@8` | Yes (stdcall) | `tsdev_lib.cpp:306` |
| `_OrlibExit@0` | Yes (stdcall) | `tsdev_lib.cpp:307` |
| `FindExistPos` | **No** (undecorated — confirmed via dumpbin) | `tsdev_lib.cpp:311` |
| `_CreateEntryEX@24` | Yes (stdcall) | `tsdev_lib.cpp:312` |
| `_SaveModule_New@20` | Yes (stdcall) | `tsdev_lib.cpp:313` |
| `_VerifyModule_New@20` | Yes (stdcall) | `tsdev_lib.cpp:314` |
| `_GetCompilingErrors@4` | Yes (stdcall) | `tsdev_lib.cpp:315` |
| `_CloseWarehouseForCurrentThreadAndPath@4` | Yes (stdcall) | `tsdev_lib.cpp:316–317` |

### 2.5 Hardcoded structural assumptions

| Assumption | Value | File:Line | Risk |
|------------|-------|-----------|------|
| CELCompiler object size | `0x58` (88 bytes) | `tsdev_lib.cpp:48` | Size could change in a new build |
| CELCompiler field `+0x10` | `0xa02` | `tsdev_lib.cpp:423` | Magic constant; meaning may change |
| CELCompiler field `+0x18` | `hwnd` (EDIT control) | `tsdev_lib.cpp:424–425` | Offset could change |
| CELCompiler field `+0x28` | `6` | `tsdev_lib.cpp:426` | Magic constant; meaning may change |
| TS Program directory path | `C:\Program Files (x86)\TradeStation 9.5\Program` | `tsdev_lib.cpp:31–32` | Path includes version "9.5" |
| MyWork\Bin path | `C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin` | `tsdev_lib.cpp:33–34` | Path includes version "9.5" |
| MWRKArea.wrk path | `C:\Program Files (x86)\TradeStation 9.5\MyWork\MWRKArea.wrk` | `tsdev_lib.cpp:504` | Hardcoded in deleteEntry |
| `.ell` file naming pattern | `elmodule_<whType>_<progId>.ell` | `tsdev_lib.cpp:488–489` | Pattern could change |
| `.xml` file naming pattern | `elmodule_<whType>_<progId>.xml` | `tsdev_lib.cpp:495–496` | Pattern could change |
| orlib20.dll image base | `0x10000000` (from FEASIBILITY.md §3) | N/A (used implicitly via `m_orlibBase`) | ASLR/rebase — but `LoadLibraryW` returns actual base, so this is OK |
| Support DLL list | 7 DLLs (ECL, ORELUI20, orUI20, elobjmod, elwinforms, tschartxelobjmdl, ELDebugger) | `tsdev_lib.cpp:37–45` | DLL names could change across versions |

### 2.6 Version detection

**None.** The CLI does not read the orlib20.dll version resource, file hash, PE timestamp, or any other version indicator. It does not warn if the loaded DLL differs from the tested version.

### 2.7 SEH coverage

| Call site | SEH-protected? | File:Line |
|-----------|---------------|-----------|
| `OrlibInit` | No (constructor, throws on FALSE return) | `tsdev_lib.cpp:338` |
| `FindExistPos` | No | `tsdev_lib.cpp:373` |
| `CreateEntryEX` | **Yes** | `tsdev_lib.cpp:380–387` |
| `SaveModule_New` | **Yes** | `tsdev_lib.cpp:395–400` |
| `CELCompiler ctor` | **Yes** (via `callCtor`) | `tsdev_lib.cpp:415–418` |
| `StartCompilingProcess` | **Yes** (via `callStartCompiling`) | `tsdev_lib.cpp:429–435` |
| `VerifyModule_New` + `GetCompilingErrors` | **Yes** (via `callVerifyAndErrors`) | `tsdev_lib.cpp:440–449` |
| `EndCompilingProcess` | **Yes** (via `callEndCompiling`) | `tsdev_lib.cpp:462` |
| `CELCompiler dtor` | **Yes** (via `callDtor`) | `tsdev_lib.cpp:465–466` |
| `CloseWarehouseForCurrentThreadAndPath` | **Yes** | `tsdev_lib.cpp:473–477` |
| Delete orchestrator (`FUN_101a8360`) | **Yes** (via `callDeleteOrchestrator`) | `tsdev_lib.cpp:553` |
| `InitGlobalLookup` | **Yes** (via `callInitGlobalLookup`) | `tsdev_lib.cpp:587–589` |
| `DeleteFromTree` | **Yes** (via `callDeleteFromTree`) | `tsdev_lib.cpp:597–598` |
| `GetWarehouse` | **Yes** (via `callGetWarehouse`) | N/A (resolved but not directly called in current delete flow) |
| `OrlibExit` | No (destructor) | `tsdev_lib.cpp:362` |
| `applyPersistencePatch` | No (VirtualProtect + memcpy) | `tsdev_lib.cpp:695–709` |

**Gaps:** `OrlibInit`, `FindExistPos`, `OrlibExit`, and `applyPersistencePatch` are not SEH-protected. An access violation in any of these would crash the process.

### 2.8 Regression test suite

**No automated regression suite exists.** The current tests (`test_insert.cpp`, `test_delete.cpp`, `test_modify.cpp`) are manual integration tests that must be run individually against a live TradeStation installation. There is no script or CI target that runs all tests in sequence, captures results, or compares against a baseline. There is no test that validates function resolution (e.g., "does the signature for StartCompilingProcess still match?") without performing a full INSERT/DELETE/MODIFY cycle.

---

## 3. Goal

An architecture where:

1. **Function discovery is signature-based or export-name-based** — internal (unexported) functions are found by scanning the loaded orlib20.dll image for stable byte-pattern signatures (unique instruction sequences at or near the function prologue), not by hardcoded RVA. Exported functions continue to use `GetProcAddress` but are validated against expected export-name forms (decorated vs. undecorated).

2. **Version mismatches are detected and reported clearly** — the CLI reads the orlib20.dll version resource (and/or PE timestamp, file size, hash) and compares against a known-good manifest. If the version is untested, the CLI prints a clear warning (not a silent crash) listing what was expected vs. what was found.

3. **Patches are validated against expected byte patterns before application** — before overwriting any byte in the loaded DLL, the current bytes are read and compared against the expected original pattern. If they don't match, the CLI aborts with a clear message (not a blind overwrite that corrupts code).

4. **The CLI fails loudly with a clear message rather than crashing** — every internal function call is SEH-protected. Every signature scan that fails to find a match throws a descriptive `std::runtime_error` rather than returning a null pointer that gets called. Every patch validation failure aborts with the RVA, expected bytes, and actual bytes.

5. **A regression test suite catches breakage** — an automated test target that (a) validates all signatures resolve to the expected functions (by checking surrounding bytes / function behavior), (b) validates all patch sites contain expected original bytes, (c) runs the full INSERT/DELETE/MODIFY cycle, and (d) can be run against a new orlib20.dll copy (from a `tmp/` directory) without a live TS install for the signature/patch checks.

---

## 4. TVS Decomposition

### TVS-B.1 — Catalog all hardcoded offsets, patches, and derive candidate signatures

- **Goal:** Produce a complete, validated signature catalog for every internal function and patch site in the current orlib20.dll.
- **Type:** RESEARCH
- **Scope:**
  - **Done:** For each of the 8 internal-function RVAs (§2.1) and 4 patch-site RVAs (§2.2), extract the first 16–64 bytes of the function (or surrounding bytes for patch sites) from the loaded orlib20.dll. Derive candidate byte-pattern signatures (with wildcards for relocatable operands) that are likely stable across minor rebuilds. Document the expected original bytes at each patch site. For each signature, verify uniqueness within the orlib20.dll .text section (no false matches). Document the calling convention, argument count, and any structural assumptions (CELCompiler size, field offsets) alongside the signatures.
  - **NOT done:** No code changes. No implementation of a scanner. No version-detection logic. No test suite. Only the catalog document is produced.
- **Acceptance criteria:**
  1. A document `docs/SIGNATURE_CATALOG.md` exists and lists, for each of the 12 RVA targets: the RVA, the function/patch name, the candidate signature (hex bytes with wildcard notation), the signature length, a uniqueness-verification result (scan result count = 1 within .text), the expected original bytes at the patch site (for patches), the calling convention, and any structural assumptions.
  2. Each signature is verified unique by scanning the actual orlib20.dll .text section (using a Frida script or a standalone probe that reads the loaded image).
  3. For exported functions (§2.4), the catalog notes the decorated/undecorated name and confirms it appears in the export table.
  4. The catalog notes the orlib20.dll file version, PE timestamp, file size, and SHA-256 hash of the currently-tested DLL (baseline for version detection).
- **Dependencies:** None (first TVS).
- **Adversarial review:** Yes — an independent reviewer verifies each signature is unique, correctly anchored, and that the catalog is complete (no RVA from §2.1/§2.2 is missing).
- **Effort:** M

### TVS-B.2 — Investigate version-detection strategies and orlib20.dll variability

- **Goal:** Determine the best method for detecting orlib20.dll version and assess how much orlib20.dll varies across available TS versions (if multiple are accessible).
- **Type:** RESEARCH
- **Scope:**
  - **Done:** Investigate available version indicators: (a) PE version resource (`VS_VERSIONINFO`), (b) PE timestamp from the COFF header, (c) file size, (d) SHA-256 hash, (e) export-table ordinal count, (f) .text section size. Determine which indicators are most stable and most discriminating. If multiple orlib20.dll versions are available (e.g., from TS update packages, backup copies, or the current install), compare their signatures from TVS-B.1 to assess how much byte patterns shift. Document the recommended version-detection approach (single hash? version resource + file size? multi-factor?). If only one version is available, document that the variability assessment is limited and the version-detection design must be conservative (treat any unknown version as "untested — proceed at your own risk").
  - **NOT done:** No implementation. No code changes. Only a research document.
- **Acceptance criteria:**
  1. A document section in `docs/SIGNATURE_CATALOG.md` (or a separate `docs/VERSION_DETECTION.md`) describes the recommended version-detection method with rationale.
  2. The document lists the baseline version indicators (version string, PE timestamp, file size, hash) for the current orlib20.dll.
  3. If multiple versions are available, the document includes a signature-stability comparison table. If not, it explicitly states the limitation and recommends a conservative fallback.
  4. The document recommends a "known-versions manifest" format (e.g., JSON or a C++ struct) that maps version indicators to expected signatures.
- **Dependencies:** TVS-B.1 (needs the signature catalog to compare across versions).
- **Adversarial review:** Yes — an independent reviewer verifies the version-detection method is sound, the baseline indicators are correctly extracted, and the limitations are honestly stated.
- **Effort:** S

### TVS-B.3 — Design the OrlibResolver abstraction layer

- **Goal:** Produce a detailed design (header + interface specification) for an `OrlibResolver` class that encapsulates all version-specific knowledge and replaces the hardcoded RVAs.
- **Type:** RESEARCH (design only — no implementation)
- **Scope:**
  - **Done:** Design the `OrlibResolver` class interface in a new header `src/orlib_resolver.h`. The class is responsible for: (a) loading orlib20.dll and capturing the base address, (b) detecting the DLL version (using the method from TVS-B.2), (c) resolving every internal function by signature scan (using the catalog from TVS-B.1) with a fallback to a known-versions manifest, (d) validating patch sites (expected original bytes) before allowing patches, (e) providing typed function pointers to the caller. The design must specify: the class API (methods, return types, error handling), the signature-scan algorithm (how wildcards work, how uniqueness is enforced, what happens on no-match), the version-manifest format, and how the class integrates with the existing `TsDevLib` constructor. Produce the header file with full Doxygen-style comments and a design rationale document section.
  - **NOT done:** No `.cpp` implementation. No modification to `tsdev_lib.cpp` or `tsdev_lib.h`. No test code. The header is a design artifact only (may contain stubs that throw `std::runtime_error("not implemented")`).
- **Acceptance criteria:**
  1. `src/orlib_resolver.h` exists with a complete class interface (public methods, private members, typedefs).
  2. Every function from §2.1 and every patch from §2.2 has a corresponding resolution method or is covered by a generic `resolveBySignature(name, signature)` method.
  3. The interface includes `validatePatchSite(name, rva, expectedBytes, patchBytes)` or equivalent.
  4. The interface includes `detectVersion()` returning a structured version descriptor.
  5. A design-rationale section in the header (or a companion `docs/ORLIB_RESOLVER_DESIGN.md`) explains: why signature scanning over RVA, how the version manifest works, how errors are reported, and how the class is unit-testable without a live TS install.
  6. The design does NOT break the existing `TsDevLib` API — it is an internal refactor.
- **Dependencies:** TVS-B.1 (signatures), TVS-B.2 (version detection method).
- **Adversarial review:** Yes — an independent reviewer verifies the interface is complete (covers all 12 RVA targets + 8 exports), the error-handling strategy is sound, and the design is testable.
- **Effort:** M

### TVS-B.4 — Implement the signature scanner and version detector

- **Goal:** Implement the `OrlibResolver` class with working signature scanning and version detection, validated by unit tests that run against a copy of the current orlib20.dll (no live TS operations).
- **Type:** IMPLEMENTATION
- **Scope:**
  - **Done:** Implement `src/orlib_resolver.cpp` with: (a) `loadDll()` — loads orlib20.dll from a given path (or the TS Program dir), captures base address, reads PE headers to locate .text section bounds. (b) `detectVersion()` — reads the PE version resource, file size, and hash; compares against the known-versions manifest; returns a version descriptor with a `known`/`unknown` flag. (c) `resolveBySignature(name, signature, mask)` — scans the .text section for the byte pattern (with wildcard mask), enforces exactly 1 match, returns the resolved address. (d) `resolveExport(decoratedName)` — wraps `GetProcAddress` with error handling. (e) `validatePatchSite(address, expectedBytes)` — reads current bytes, compares, returns bool + diagnostic. (f) Unit tests (`test_orlib_resolver.cpp`) that load a COPY of orlib20.dll from `tmp/` (not the live install), run all signature scans, verify all resolve to the expected RVAs (from §2.1/§2.2), verify all patch sites contain expected original bytes, and verify version detection returns `known` for the current DLL.
  - **NOT done:** No integration with `TsDevLib` yet (that's TVS-B.5). No modification to `tsdev_lib.cpp`. No INSERT/DELETE/MODIFY tests. The resolver is standalone and tested in isolation.
- **Acceptance criteria:**
  1. `src/orlib_resolver.cpp` implements all methods from the TVS-B.3 interface.
  2. `test_orlib_resolver.cpp` compiles, links, and runs against a copied orlib20.dll in `tmp/`.
  3. All 8 internal-function signatures resolve to addresses matching the current RVAs (base + RVA from §2.1).
  4. All 4 patch-site signatures resolve correctly and `validatePatchSite` confirms expected original bytes.
  5. `detectVersion()` returns `known=true` for the current orlib20.dll.
  6. A deliberate signature mismatch (corrupted copy) causes `resolveBySignature` to throw a clear `std::runtime_error` with the signature name and match count.
  7. All tests pass with exit code 0.
- **Dependencies:** TVS-B.1 (signatures), TVS-B.2 (version detection), TVS-B.3 (interface design).
- **Adversarial review:** Yes — an independent reviewer verifies the scanner is correct (no off-by-one in .text bounds, wildcard logic is sound), the tests actually exercise failure paths, and the version detection is not trivially spoofable.
- **Effort:** L

### TVS-B.4.5 — Resolve `resolveErrorHinstRva` hardcoded fallback (instruction-offset extraction) — DONE & APPROVED

- **Status:** **DONE & APPROVED**
- **Goal:** Replace the hardcoded `0x23AAB8` fallback in `resolveErrorHinstRva()` with real instruction-offset extraction, matching the pattern of `resolveErrorArrayPtrRva()` and `resolveErrorCountRva()`. This completes the robustness of all 3 data-global RVA resolvers.
- **Type:** IMPLEMENTATION
- **Rationale:** B.4 left `resolveErrorHinstRva` as a hardcoded fallback because the Ghidra evidence did not identify which resolved function references `DAT_1023aab8`. This TVS closes that gap so B.5 integrates a fully-robust resolver (no hardcoded RVAs remain).
- **Summary of work completed:**
  - Replaced the hardcoded `0x23AAB8` fallback in `resolveErrorHinstRva()` with instruction-offset extraction from the `_CreateEntryEX@24` export at offset `0x6C` (opcode `8B 0D`, `MOV ECX,[imm32]`).
  - All 3 data-global RVA resolvers now use instruction-offset extraction — no hardcoded RVAs remain in the resolver.
  - Ghidra evidence: `tmp/re_artifacts/b4_5_hinst_xref.md`.
  - Files changed: `src/orlib_resolver.cpp` (method body + strategy comments), `src/orlib_resolver.h` (Doxygen comment only).
  - Verification: `test_orlib_resolver.exe` exit 0, `sig_scan.exe` exit 0.
  - Adversarial review: first DISAPPROVED (1 MAJOR stale comment + 1 MINOR), fix applied, re-review APPROVED.
- **Scope:**
  - **Done:** (a) Ghidra MCP analysis to find which resolved function references `DAT_1023aab8` (the string-table HINSTANCE at RVA 0x23AAB8) and at what instruction offset within that function. Note: `DAT_1023aab8` has 193 xrefs across many functions; the task is to find a reference from one of the 8 resolved internal functions (StartCompilingProcess, EndCompilingProcess, CELCompilerCtor, CELCompilerDtor, InitGlobalLookup, DeleteFromTree, GetWarehouse, DeleteOrchestrator) or one of the 7 resolved exports. If none of the resolved functions reference it, the scope expands to resolving a NEW referencing function by signature — flag that to the PM. (b) Update `resolveErrorHinstRva()` in `src/orlib_resolver.cpp` to use instruction-offset extraction (opcode verification + imm32 extraction + subtract image base), matching the pattern of `resolveErrorArrayPtrRva()` and `resolveErrorCountRva()`. (c) Remove the TODO/hardcoded-fallback comment. (d) Re-verify: `test_orlib_resolver.exe` still passes (expected RVA 0x23AAB8 unchanged), `sig_scan.exe` regression, adversarial review.
  - **NOT done:** No `tsdev_lib.cpp` changes (that's B.5). No `test_orlib_resolver.cpp` changes (expected RVA 0x23AAB8 stays the same). No new CLI features.
- **Acceptance criteria:**
  1. `resolveErrorHinstRva()` uses instruction-offset extraction (no hardcoded RVA). ✅
  2. Returns `0x23AAB8` for the baseline DLL (test passes unchanged). ✅
  3. No TODO/hardcoded-fallback comments remain. ✅
  4. `test_orlib_resolver.exe` exit 0, `sig_scan.exe` exit 0. ✅
  5. Adversarial review APPROVED. ✅
- **Dependencies:** TVS-B.4 (DONE & APPROVED). Ghidra MCP (PM performs calls directly).
- **Adversarial review:** Yes — an independent reviewer verifies the instruction-offset extraction is correct (opcode verification + imm32 + base subtraction), the Ghidra evidence is sound, no hardcoded RVA remains, and all tests pass with identical behavior. **Result: APPROVED** (after one DISAPPROVE→fix→re-approve loop).
- **Effort:** S
- **Scheduling:** BEFORE B.5 (so B.5 integrates a fully-robust resolver with no hardcoded RVAs remaining). — **COMPLETE; next TVS is B.5.**

### TVS-B.5 — Integrate OrlibResolver into TsDevLib and add patch-site validation — DONE & APPROVED

- **Status:** **DONE & APPROVED**
- **Goal:** Replace all hardcoded RVA constants in `tsdev_lib.cpp` with `OrlibResolver` calls. Add pre-patch byte validation to `applyPersistencePatch()`. Add SEH wrappers to the remaining unprotected call sites.
- **Type:** IMPLEMENTATION
- **Summary of work completed:**
  - Replaced all 15 hardcoded RVA constants in `src/tsdev_lib.cpp` with `OrlibResolver` calls (8 internal functions via signature scan, 7 exports via GetProcAddress, 3 data-global RVAs via instruction-offset extraction, 4 patch sites via signature scan).
  - Added 3 SEH wrappers (`callOrlibInit`, `callOrlibExit`, `callFindExistPos`) — POD-only helper functions using `__try`/`__except` (avoids MSVC C2712). Protects OrlibInit (constructor), OrlibExit (destructor), and FindExistPos (findExistPos + deleteEntry).
  - Added patch restoration on destruction (new — not in original spec): destructor restores all 4 patch sites to original bytes AFTER OrlibExit but BEFORE resolver destruction. Fixes a regression where a second TsDevLib instance in the same process failed.
  - Fixed a crash: `validatePersistencePatchBytes()` rewritten to use a cached RVA (`m_persistencePatchRva`) instead of re-scanning for original bytes (already patched by construction time). The re-scan caused an `OrlibResolverError` to escape to WerFault.
  - Changed `applyPersistencePatch()` and `applyThreadCheckPatches()` from `bool` to `void` (private methods). `OrlibResolverError` propagates on failure. Removed 4 duplicate anonymous-namespace typedefs (collide with `orlib_resolver.h`).
  - TDD red-green: `test_no_hardcoded_rvas` (exit 1→0). Full regression: all 7 tests pass (exit 0).
  - Adversarial review: first DISAPPROVED (1 MAJOR: unprotected FindExistPos in deleteEntry — AC3 violation), fix applied (routed through SEH wrapper), re-review APPROVED. 3 MINOR findings (non-blocking).
  - Files changed: `src/tsdev_lib.h`, `src/tsdev_lib.cpp`, `src/test_no_hardcoded_rvas.cpp` (new), `CMakeLists.txt`.
  - Known limitation: if `applyThreadCheckPatches()` throws midway, already-applied patches won't be restored (very low likelihood). Tracked as a follow-up for a future robustness TVS — see this handoff's historical limitation record.
- **Scope:**
  - **Done:** (a) Modify `TsDevLib` constructor to construct an `OrlibResolver` first, use it to resolve all 8 internal functions and validate all 4 patch sites before proceeding. (b) Remove the hardcoded RVA constants (`kRvaStartCompiling` through `kRvaDeleteOrchestrator`, `kRvaPersistencePatch`, `kRvaThreadCheck1–3`) from `tsdev_lib.cpp`. (c) Add byte-validation to `applyPersistencePatch()` — read the 8 bytes at the patch site, compare against the expected original function prologue (from TVS-B.1 catalog), abort with a clear message if they don't match. (d) Add SEH wrappers around `OrlibInit` (in constructor) and `OrlibExit` (in destructor) and `FindExistPos`. (e) Add a version check at constructor start: if `detectVersion()` returns `unknown`, print a clear warning to stderr but proceed (do not abort — the signatures may still match). If signature resolution fails, throw with a clear message. (f) Run existing `test_insert`, `test_delete`, `test_modify` and confirm they still pass.
  - **NOT done:** No new CLI features. No new orlib20 functions. No change to the public `TsDevLib` API. No change to test logic — only the resolution mechanism changes under the hood.
- **Acceptance criteria:**
  1. `tsdev_lib.cpp` contains zero hardcoded RVA constants (grep for `0x1` in the anonymous namespace returns nothing related to RVAs). ✅
  2. `applyPersistencePatch()` reads and validates original bytes before writing; on mismatch, throws `std::runtime_error` with expected vs. actual bytes. ✅
  3. `OrlibInit`, `OrlibExit`, and `FindExistPos` are SEH-protected. ✅
  4. Constructor prints a version-detection warning to stderr if the DLL is unknown. ✅
  5. `test_insert`, `test_delete`, `test_modify` all pass with exit code 0 (behavior unchanged). ✅
  6. A deliberate bad signature (temporarily corrupt one signature in the manifest) causes the constructor to throw a clear error instead of crashing. ✅
- **Dependencies:** TVS-B.4 (resolver implementation). — **DONE & APPROVED**
- **Adversarial review:** Yes — an independent reviewer verifies no hardcoded RVAs remain, patch validation is correct, SEH wrappers are sound, and all existing tests pass with identical behavior. **Result: APPROVED** (after one DISAPPROVE→fix→re-approve loop).
- **Effort:** M

### TVS-B.6 — Build the automated regression test suite

> **Workstream E integration:** After Workstream E is complete, the B.6 regression suite should include `sigtool verify` (signature uniqueness verification) as an additional test gate. See `docs/ARCHITECTURE_REDESIGN.md` §5.1 for sigtool's command interface.

- **Goal:** Create a single automated test target that runs all validation checks and integration tests in sequence, with pass/fail reporting, suitable for running against a new orlib20.dll.
- **Type:** IMPLEMENTATION
- **Scope:**
  - **Done:** (a) Create `test_regression.cpp` (or a test runner script) that executes in order: (1) `test_orlib_resolver` — signature resolution + patch-site validation + version detection (from TVS-B.4, can run against a copied DLL without live TS), (2) `test_insert` — full INSERT integration (requires live TS), (3) `test_delete` — full DELETE integration, (4) `test_modify` — full MODIFY integration. (b) The runner captures exit codes, prints a summary table (test name, pass/fail, exit code, duration), and returns non-zero if any test fails. (c) Add a CMake target `test_regression` that builds all test executables. (d) Add a `run_regression.ps1` (or `.bat`) script that runs the suite and prints the summary. (e) Document in `docs/BUILD.md` how to run the regression suite.
  - **NOT done:** No CI/CD pipeline (out of scope — manual execution only). No test against an actual new orlib20.dll version (that's the point — the suite is ready for when one appears). No mocking framework.
- **Acceptance criteria:**
  1. `test_regression` CMake target builds successfully.
  2. `run_regression.ps1` (or `.bat`) runs all 4 test executables, captures exit codes, and prints a summary table.
  3. The suite passes (all green) against the current orlib20.dll.
  4. The suite correctly reports failure when a test exits non-zero (verified by temporarily breaking one test).
  5. The signature/patch portion (test_orlib_resolver) can run standalone without a live TS install (uses a copied DLL from `tmp/`).
  6. `docs/BUILD.md` documents how to run the suite.
- **Dependencies:** TVS-B.4 (resolver tests), TVS-B.5 (integrated TsDevLib).
- **Adversarial review:** Yes — an independent reviewer verifies the suite is complete, the runner correctly captures failures, and the documentation is accurate.
- **Effort:** S

### TVS-B.7 — Path discovery and structural-constant externalization — DONE & APPROVED

**STATUS: COMPLETE & APPROVED (2026-08-18)**

> **PARTIAL SUPERSESSION by Workstream E:** Items (c) and (d) below are superseded by Workstream E's JSON version manifest (`signatures/version_manifest.json` → codegen → generated header). Structural constants and file-naming patterns will be externalized as part of TVS-E.1/E.3. B.7's remaining scope is items (a) and (b) only.

- **Status:** **DONE & APPROVED**
- **Goal:** Replace hardcoded TS install paths and structural constants (CELCompiler size, field offsets, file-naming patterns) with discovered or configurable values, reducing another class of version fragility.
- **Type:** IMPLEMENTATION
- **Summary of work completed (RESULT):**
  - **Items (a) and (b) DONE:** TS install path discovery (registry + `--ts-path` CLI override + conventional default from env var) and `MyWork\Bin` / `MWRKArea.wrk` derived from the discovered install root.
  - **Items (c) and (d) SUPERSEDED by Workstream E** (as noted above) — structural constants and file-naming patterns will be externalized via E's JSON version manifest.
  - **Path discovery chain:** registry (`SOFTWARE\TradeStation Technologies\TradeStation\Installing\RootDirectory`, REG_SZ, opened with `KEY_WOW64_32KEY` — no `WOW6432Node` in the path, avoiding double-redirection) → fallback registry key (`Versions\9.05.00\RootDirectory`) → conventional default (`GetEnvironmentVariableW(L"ProgramFiles(x86)")` + `L"\\TradeStation 9.5"` — NOT a hardcoded absolute path).
  - **TsDevLib constructor:** `explicit TsDevLib(const wchar_t* tsPathOverride = nullptr)` — backward compatible (defaults to `nullptr` → full discovery chain).
  - **New getters:** `getMyWorkBinPath()`, `getWrkPath()` — derived from the install root.
  - **CLI:** `--ts-path <dir>` global flag (space and `=` forms accepted; empty-value rejected with a clear error).
  - **Removed 4 hardcoded paths:** 3 from `src/tsdev_lib.cpp` (TS Program dir, MyWork\Bin, MWRKArea.wrk) + 1 from `tests/test_delete_diag.cpp`.
  - **AC1 relaxed (PM decision):** no hardcoded absolute path `C:\Program Files (x86)\TradeStation 9.5` remains; the literal "TradeStation 9.5" appears only as a path component of the conventional default (built from the `ProgramFiles(x86)` env var), not as a hardcoded full path.
  - **New test:** `tests/test_path_discovery.cpp` (5 cases: registry discovery, override respected, fallback chain, env-var default, empty-value rejection).
  - **Verification:** TDD red-green confirmed; 11/11 regression pass; adversarial review APPROVED (0 CRITICAL/MAJOR); E2E CLI 11/11 PASS.
  - **Files changed:** `src/tsdev_lib.h`, `src/tsdev_lib.cpp`, `src/tsdev_cli.cpp`, `tests/test_delete_diag.cpp`, `tests/test_path_discovery.cpp` (NEW), `CMakeLists.txt`.
- **Scope:**
  - **Done:** (a) Replace the hardcoded TS Program directory path with discovery logic: check the registry (e.g., `HKLM\SOFTWARE\WOW6432Node\TradeStation\...` install path key) or scan common paths, with a `--ts-path` CLI override. (b) Derive `MyWork\Bin` and `MWRKArea.wrk` paths from the discovered install root rather than hardcoding "9.5". (c) Move CELCompiler structural constants (size `0x58`, field offsets `+0x10`/`+0x18`/`+0x28`, magic values `0xa02` and `6`) into the `OrlibResolver`'s version manifest so they can be overridden per-version. (d) Make the `.ell`/`.xml` file-naming pattern configurable (or at least documented as a version-dependent assumption in the manifest). (e) Run existing tests to confirm no behavioral change.
  - **NOT done:** No registry-key reverse engineering beyond a quick lookup. No support for non-9.5 TS versions (the discovery just makes the path non-hardcoded; actual multi-version support requires the signatures to match, which is TVS-B.1's concern). No CLI argument parsing framework (just a simple `--ts-path` override).
- **Acceptance criteria:**
  1. No hardcoded "TradeStation 9.5" path string exists in `tsdev_lib.cpp` (grep returns nothing). ✅ (relaxed per PM decision — see RESULT)
  2. The TS install path is discovered from registry or overridable via CLI argument. ✅
  3. CELCompiler structural constants are read from the version manifest (with current values as defaults). — SUPERSEDED by Workstream E (items c/d).
  4. All existing tests pass with exit code 0. ✅
  5. A `--ts-path` override to a different directory is respected (verified by pointing to the same dir and confirming tests pass). ✅
- **Dependencies:** TVS-B.5 (OrlibResolver integrated, version manifest exists). — **DONE & APPROVED**
- **Adversarial review:** Yes — an independent reviewer verifies no hardcoded paths remain, the discovery logic is sound, and structural constants are properly externalized. **Result: APPROVED** (0 CRITICAL/MAJOR).
- **Effort:** M

---

## 5. Research Interleaving

The TVSs are ordered so that research precedes the implementation that depends on it:

```
TVS-B.1 (RESEARCH — signature catalog)
    │
    ├──► TVS-B.2 (RESEARCH — version detection strategy)
    │       │
    │       └──► TVS-B.3 (RESEARCH — OrlibResolver design)
    │               │
    │               └──► TVS-B.4 (IMPL — signature scanner + version detector)
    │                       │
    │                       └──► TVS-B.4.5 (IMPL — fix resolveErrorHinstRva hardcoded fallback) ✅ DONE & APPROVED
    │                               │
    │                               └──► TVS-B.5 (IMPL — integrate into TsDevLib) ✅ DONE & APPROVED
    │                                       │
    │                                       ├──► TVS-B.6 (IMPL — regression suite) ← AFTER WORKSTREAM E
    │                                       │
    │                                       └──► TVS-B.7 (IMPL — path/constant externalization) ✅ DONE & APPROVED (items a+b; c/d superseded by E)
    │
    └── (TVS-B.2 also depends on B.1 for signatures to compare across versions)

Workstream E (ARCHITECTURE_REDESIGN — JSON manifest + sigtool + codegen)
    │
    ├──► TVS-B.6 (prerequisite — B.6 regression suite should include `sigtool verify`)
    │
    └──► TVS-B.7 (prerequisite — E supersedes B.7 items (c) and (d) via JSON version manifest) ✅ DONE & APPROVED (items a+b)
```

**Key ordering constraints:**
- TVS-B.1 MUST complete before TVS-B.2 (needs signatures to compare) and before TVS-B.3 (needs signatures to design the resolver interface).
- TVS-B.2 MUST complete before TVS-B.3 (version-detection method informs the resolver design).
- TVS-B.3 MUST complete before TVS-B.4 (interface design precedes implementation).
- TVS-B.4 MUST complete before TVS-B.4.5 (resolver implementation must exist before fixing its last hardcoded fallback).
- TVS-B.4.5 MUST complete before TVS-B.5 (B.5 integrates a fully-robust resolver with no hardcoded RVAs remaining). — **B.4.5 DONE & APPROVED; B.5 is now unblocked (next TVS).**
- TVS-B.5 MUST complete before TVS-B.6 (regression suite tests the integrated system) and TVS-B.7 (path externalization touches the integrated code). — **B.5 DONE & APPROVED; B.7 DONE & APPROVED (items a+b). B.6 is unblocked from B's chain but should wait for Workstream E.**
- TVS-B.6 and TVS-B.7 are independent of each other and can be done in parallel after TVS-B.5. — **B.5 complete; B.7 DONE & APPROVED (items a+b). Only B.6 remains (after Workstream E).**
- Workstream E should be executed before B.6 and B.7. E provides the `sigtool` infrastructure that B.6's regression suite should include, and E externalizes the structural constants that B.7 item (c) was intended to address.

---

## 6. Risk Register

| ID | Risk | Likelihood | Impact | Mitigation |
|----|------|-----------|--------|------------|
| R1 | Signatures are not unique within .text | Medium | High — wrong function resolved, crash | TVS-B.1 enforces uniqueness verification (scan count = 1). If a signature is not unique, extend it with more bytes or add a relative-offset anchor (e.g., "N bytes after a unique preceding call"). |
| R2 | orlib20.dll is packed/encrypted at rest | Low | Critical — signature scanning of the on-disk file fails | Mitigation: scan the LOADED image (after the OS loader unpacks it), not the on-disk file. The current approach already loads via `LoadLibraryW`. |
| R3 | Future orlib20 versions change calling conventions | Low | High — wrong ABI, crash or silent corruption | The version manifest records calling conventions per version. If a new version is detected, the CLI warns and the signature scan includes prologue bytes that encode the calling convention (e.g., `push ebp; mov ebp,esp` vs. `sub esp,N`). |
| R4 | Signatures match but function semantics changed | Medium | High — function found but behaves differently, silent data corruption | The regression suite (TVS-B.6) catches behavioral changes. The version manifest marks unknown versions as "untested." No silent fallback to untested behavior. |
| R5 | CELCompiler object layout changes across versions | Medium | Medium — field offsets wrong, compile fails or corrupts | ~~TVS-B.7 externalizes structural constants into the version manifest.~~ **RESOLVED (partially):** B.7 items (c)/(d) superseded by Workstream E's JSON version manifest. The signature for the CELCompiler constructor includes the `mov [ecx+N], value` instructions that reveal field offsets. |
| R6 | Only one orlib20.dll version is available for analysis | High | Medium — signature stability across versions is unverified | TVS-B.2 explicitly documents this limitation. The design is conservative: unknown versions get a warning, not a silent proceed. The signature catalog uses minimal-length signatures (fewer assumptions about surrounding code). |
| R7 | TS install path changes in future versions (e.g., "TradeStation 10") | Medium | Low — CLI can't find the DLL | **RESOLVED by TVS-B.7 (DONE & APPROVED):** registry-based discovery (`SOFTWARE\TradeStation Technologies\TradeStation\Installing\RootDirectory` with `KEY_WOW64_32KEY`) + `--ts-path` CLI override + conventional default from `ProgramFiles(x86)` env var. |
| R8 | Export names change (decoration changes, renames) | Low | Medium — `GetProcAddress` fails, clear error | Export resolution already throws on failure. The version manifest can record alternate export names per version. |
| R9 | `applyPersistencePatch` original bytes change | Medium | Medium — patch validation fails, CLI aborts (not a crash) | This is the desired behavior — fail loudly. The version manifest records the expected original bytes per version. |
| R10 | SEH wrappers mask real bugs by swallowing exceptions | Low | Medium — silent failure instead of crash | SEH wrappers log to stderr before returning. The regression suite checks for unexpected SEH catches. |
| R11 | Support DLL list changes across versions | Low | Low — pre-load warnings, OrlibInit may fail | The support DLL list is non-fatal (warnings only). The version manifest can record the expected list. |

---

## 7. Constraints

1. **Standalone** — no TSDev.exe required. All operations work via direct orlib20.dll invocation in the CLI's own process.
2. **No WHServer killing** — the CLI must not terminate WHServer.exe or any other TradeStation process.
3. **READ-ONLY TS install** — the on-disk `orlib20.dll` and all TS installation files must never be modified. Binary patches are applied only to the in-memory loaded image via `VirtualProtect` + `memcpy`. Copies of DLLs for testing go into `tmp/`.
4. **32-bit** — the CLI and all test executables are 32-bit (x86) to load the 32-bit orlib20.dll. Enforced by the CMake guard (`CMAKE_SIZEOF_VOID_P == 4`).
5. **Adversarial review required** — every TVS result (research document, implementation, test) must be adversarially reviewed by an independent subagent per the AGENTS.md rules. The reviewer checks for complete execution, no scope creep, no drift, and no undue shortcuts.
6. **TDD** — implementation TVSs follow the red-green method: write the test first, confirm it fails, then implement until it passes.
7. **No scope creep** — each TVS is narrowly scoped. Do not combine concerns. Do not refactor beyond the TVS scope.
8. **One subagent → one subtask** — per AGENTS.md, each TVS (or subtask within a TVS) is executed by a single focused subagent.

---

## 8. Execution Notes for the Implementing Team

- **Read first:** `AGENTS.md` (project rules), this document, `src/tsdev_lib.h` + `src/tsdev_lib.cpp` (current implementation), `docs/SIGNATURE_CATALOG.md` (produced by TVS-B.1).
- **Build:** `cmake -S . -B build -A Win32 && cmake --build build --config Release` (see `docs/BUILD.md`).
- **Test execution:** Integration tests (`test_insert`, `test_delete`, `test_modify`) require NO TradeStation process/service running (verified headless). The resolver unit test (`test_orlib_resolver`) can run against a copied DLL in `tmp/` without any TS dependency.
- **Frida:** Use Frida directly via the `frida` CLI for any live RE needed during TVS-B.1/B.2 (e.g., extracting function bytes, verifying signatures). Never use Frida MCP tools.
- **Ghidra:** Use the Ghidra MCP server ("ghidra") for static analysis (decompilation, disassembly, cross-references) during TVS-B.1/B.2/B.3.
- **Copying orlib20.dll for testing:** Copy from `C:\Program Files (x86)\TradeStation 9.5\Program\orlib20.dll` to `tmp/orlib20_copy.dll` for signature scanning tests. Never modify the original.
