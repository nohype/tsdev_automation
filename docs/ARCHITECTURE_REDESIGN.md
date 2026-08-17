# Architecture Redesign — TSDev Automation Signature Infrastructure

| Field | Value |
|---|---|
| **Date** | 2026-08-17 |
| **Purpose** | Production-grade architecture for the signature management infrastructure, directory restructuring, RVA deduplication, and probe archival. Drives a sequence of incremental TVSs. |
| **Scope** | Architecture design document only — no code changes. |
| **Inputs** | `docs/SIGNATURE_INFRASTRUCTURE_REPORT.md`, `docs/SIGNATURE_CATALOG.md`, `docs/VERSION_DETECTION.md`, `src/orlib_resolver.h`, `src/orlib_resolver.cpp`, `CMakeLists.txt`, `tmp/re_artifacts/b1_signatures.h`, `src/test_no_hardcoded_rvas.cpp`, `src/test_orlib_resolver.cpp` |

---

## 1. Executive Summary

The TSDev Automation project reverse-engineers TradeStation's `orlib20.dll` to automate TSDev Library management via a CLI. The current architecture has a critical defect: the production CLI depends on `tmp/re_artifacts/b1_signatures.h` — a manually-transcribed file in a gitignored directory — so a fresh clone cannot build. Additionally, 15 RVAs are duplicated across 6+ locations, 26 experimental probe files (27 CMake targets, including one duplicate target compiling the same source) are mixed into production source, and no tooling exists to regenerate signatures when the target DLL changes.

The target architecture moves signature data from a gitignored C++ header into committed JSON files (the source of truth), generates a C++ header at build time via `sigcodegen.py` (zero runtime overhead, no JSON dependency in C++), provides a `sigtool` CLI for the "try existing first" update workflow, and archives experimental probes outside `src/`. Adding a new DLL version becomes a well-defined additive operation: run `sigtool verify`, optionally regenerate via Ghidra headless, add a JSON file + manifest entry, commit.

Key design decisions: (1) JSON as source of truth with build-time C++ code generation; (2) multi-version manifest mapping DLL versions to signature sets; (3) experimental probes archived to `archive/` and removed from the production CMake build; (4) Ghidra headless auto-generation with `getOperandValueMask` wildcarding; (5) incremental migration via TVSs that leave the project buildable after each step.

---

## 2. Current State Analysis

### 2.1 The Build Dependency Crisis

The production code chain is:

```
src/tsdev_cli.cpp → links tsdev_lib → links orlib_resolver → #include "b1_signatures.h"
```

`b1_signatures.h` lives at `tmp/re_artifacts/b1_signatures.h` (line 34 of `src/orlib_resolver.cpp`: `#include "b1_signatures.h"`). The `tmp/` directory is gitignored. A fresh clone has no `b1_signatures.h` and cannot build `orlib_resolver`, `tsdev_lib`, `tsdev_cli`, `sig_scan`, `test_orlib_resolver`, or any test that links `tsdev_lib`.

The file is not regenerable by any script — it was manually transcribed from Ghidra disassembly. Its `SignatureDef` struct (lines 29–39) and `kSignatures[]` array (lines 48–211) encode 12 byte-signatures with hex patterns, RVAs, patch metadata, and containing-function names.

### 2.2 RVA Duplication

The same 15 RVAs (12 code-section + 3 data-section) appear in:

| Location | File | Lines | Form |
|---|---|---|---|
| Signature definitions | `tmp/re_artifacts/b1_signatures.h` | 48–211 | `rva` field in `SignatureDef`† |
| Resolver header comments | `src/orlib_resolver.h` | 554, 559, 564, 569, 574, 579, 590, 600, 748–778 | Doxygen comments mentioning RVAs |
| TsDevLib header comments | `src/tsdev_lib.h` | (various) | Historical comments |
| Test assertions | `src/test_orlib_resolver.cpp` | 180–366, 525–701, 1206–1398 | Hardcoded `0x165DE0` etc. in `CHECK` macros† |
| No-hardcoded-RVA test | `src/test_no_hardcoded_rvas.cpp` | 36–52 | `kForbidden[]` array of 15 hex strings† |
| Elevated tests | `src/elevated_test*.cpp` | (various) | Hardcoded RVA arithmetic |

**Note:** Rows marked † document duplication that has already been eliminated by Workstream E: `b1_signatures.h` (removed by TVS-E.1 — signature data migrated to `signatures/*.json` + build-time codegen), `test_orlib_resolver.cpp` hardcoded RVA literals (removed by TVS-E.3 — now derived from `signatures.gen.h`), and `test_no_hardcoded_rvas.cpp` `kForbidden[]` (removed by TVS-E.4 — forbidden list now derived from `signatures.gen.h`). The table above records the pre-redesign duplication that motivated this work; remaining unmarked rows (resolver/TsDevLib header comments, elevated tests) are addressed by TVS-E.5/E.7.

### 2.3 Probe Pollution

26 experimental probe files (`elevated_test.cpp` through `elevated_test21.cpp`, `hook_dll.cpp`, `inject.cpp`, `live_probe.cpp`, `tsdev_probe.cpp`, `sig_scan.cpp`) are compiled as CMake targets alongside production code (27 CMake targets, including one duplicate target compiling the same source; CMakeLists.txt lines 31–57). They pollute the build with targets that have no production purpose, confuse the build graph, and make the project appear larger than it is.

### 2.4 What Already Works

The production code (`tsdev_lib.cpp`) is already de-coupled from hardcoded RVAs — it uses `OrlibResolver` for all internal-function resolution. The resolver (`orlib_resolver.cpp`, 1495 lines) implements PE parsing, signature scanning, export resolution, patch-site validation/application, and data-global RVA extraction. The interface (`orlib_resolver.h`, 935 lines) is well-designed with test seams (in-memory buffer constructor, protected virtual `scanTextSection`). The problem is purely in the data layer: the resolver reads from a gitignored, non-regenerable header.

---

## 3. Target Directory Structure

```
tsdev_automation/
├── CMakeLists.txt                    # Root build — production targets + codegen
├── AGENTS.md                         # Project rules (existing)
├── .gitignore                        # Updated: build/, tmp/ still ignored; signatures/ committed
│
├── src/                              # PRODUCTION SOURCE CODE ONLY
│   ├── tsdev_cli.cpp                 # CLI entry point (existing, unchanged)
│   ├── tsdev_lib.h                   # Engine interface (existing, comments cleaned)
│   ├── tsdev_lib.cpp                 # Engine implementation (existing, unchanged logic)
│   ├── orlib_resolver.h              # Resolver interface (existing, comments cleaned)
│   ├── orlib_resolver.cpp            # Resolver implementation (modified: includes signatures.gen.h)
│   └── sigtool.cpp                   # NEW: standalone signature verification CLI
│
├── signatures/                       # SIGNATURE DATA — source of truth (committed to git)
│   ├── README.md                     # How to update signatures (workflow guide)
│   ├── version_manifest.json         # Maps DLL versions → signature set files
│   ├── v9.50.01.3344.json            # Signature set for baseline version
│   └── v<future>.json                # (future) Additional version signature sets
│
├── tools/                            # SCRIPTS AND TOOLING
│   ├── sigcodegen.py                 # JSON → C++ header code generator
│   ├── sigdiff.py                    # Diff two signature JSON files
│   └── ghidra_scripts/
│       ├── GenerateSignatures.py     # Ghidra headless: auto-extract candidate signatures
│       └── FindTargetsByAnchor.py    # (future) Locate shifted functions by string/xref anchors
│
├── tests/                            # TESTS (moved from src/)
│   ├── test_orlib_resolver.cpp       # Resolver unit tests (modified: RVAs from generated header)
│   ├── test_insert.cpp               # INSERT functional regression
│   ├── test_delete.cpp               # DELETE functional regression
│   ├── test_delete_diag.cpp          # DELETE diagnostic
│   ├── test_delete_check.cpp         # DELETE persistence check
│   ├── test_modify.cpp               # MODIFY functional regression
│   ├── test_persistence_validation.cpp # Patch byte validation
│   ├── test_no_hardcoded_rvas.cpp    # RVA deduplication guard (modified)
│   ├── fixtures/                     # Test fixtures (committed to repo)
│   │   └── orlib20_baseline.dll      # Baseline DLL copy (for offline testing)
│   └── CMakeLists.txt                # Test-specific CMake (optional: can stay in root)
│
├── archive/                          # ARCHIVED EXPERIMENTAL PROBES (not compiled)
│   ├── README.md                     # Explains what these are, why archived, how to reference
│   ├── elevated_test.cpp             # Original probe
│   ├── elevated_test2.cpp            # ...
│   ├── ... (all elevated_test*.cpp)
│   ├── elevated_test21.cpp           # Last probe
│   ├── hook_dll.cpp                  # DLL injection experiment
│   ├── inject.cpp                    # Injection launcher
│   ├── live_probe.cpp                # Live runtime probe
│   ├── tsdev_probe.cpp               # Initial smoke test probe
│   └── sig_scan.cpp                  # Standalone uniqueness scanner (reference; superseded by sigtool)
│
├── docs/                             # DOCUMENTATION
│   ├── ARCHITECTURE_REDESIGN.md      # This document
│   ├── SIGNATURE_CATALOG.md          # Human-readable catalog (existing)
│   ├── SIGNATURE_INFRASTRUCTURE_REPORT.md  # Research report (existing)
│   ├── VERSION_DETECTION.md          # Version detection design (existing)
│   ├── BUILD.md                      # Build guide (updated for new structure)
│   ├── FEASIBILITY.md                # (existing)
│   └── ... (other existing docs)
│
├── build/                            # BUILD OUTPUT (gitignored)
│   └── generated/
│       └── signatures.gen.h          # GENERATED header (not committed; built from JSON)
│
└── tmp/                              # TEMPORARY RE ARTIFACTS (gitignored)
    └── re_artifacts/
        ├── b1_signatures.h           # Original header (kept as historical reference; no longer a build dep)
        ├── b1_ghidra_evidence.md     # Ghidra disassembly evidence
        ├── b1_dll_baseline.md        # DLL baseline metadata
        └── ... (other RE artifacts)
```

### Key structural principles

1. **`src/` contains only production code.** No tests, no probes, no experimental code. Every file in `src/` is compiled into a production target (`tsdev_cli`, `tsdev_lib`, `orlib_resolver`, `sigtool`).
2. **`signatures/` is committed to git.** JSON files are the source of truth. They are diff-friendly and human-readable.
3. **`build/generated/signatures.gen.h` is NOT committed.** It is a build artifact, regenerated from JSON on every build. This is analogous to generated `.o` files — never checked in.
4. **`archive/` is not compiled.** Probes are moved here for reference. They are not CMake targets. A `README.md` explains their historical context.
5. **`tmp/` remains gitignored.** It holds the baseline DLL copy and original RE artifacts. The build no longer depends on anything in `tmp/`.
6. **Fresh clone builds.** A `git clone` + `cmake -S . -B build -A Win32` + `cmake --build build --config Release` produces a working `tsdev_cli.exe` without any `tmp/` content.

---

## 4. Signature Data Architecture

### 4.1 Signature Set JSON Schema

Each versioned signature set is a single JSON file (e.g., `signatures/v9.50.01.3344.json`). The schema:

```json
{
  "schemaVersion": 1,
  "dll": {
    "fileName": "orlib20.dll",
    "fileVersion": "9.50.01.3344",
    "sha256": "318DB24A4F41BDB9FF69E2E647026B788706904CF887CBB0A953E003DB22EC18",
    "peTimestamp": "0x5C01735E",
    "peTimestampHuman": "Fri Nov 30 18:29:02 2018 UTC",
    "fileSize": 4924400,
    "textRva": "0x1000",
    "textVsize": "0x1F72FC",
    "textRawSize": "0x1F7400",
    "imageBase": "0x10000000"
  },
  "signatures": [
    {
      "name": "StartCompilingProcess",
      "targetType": "call_target",
      "callingConvention": "cdecl",
      "expectedRva": "0x165DE0",
      "hexPattern": "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 51 8B 0D ?? ?? ?? ?? 33 C0 A3 ?? ?? ?? ?? 85 C9 74 15 51 FF 15 ?? ?? ?? ?? A3 ?? ?? ?? ?? 85 C0 75 19 A1 ?? ?? ?? ?? 69",
      "length": 64,
      "expectedMatchCount": 1,
      "provenance": {
        "author": "TVS-B.1",
        "date": "2026-08-17",
        "source": "Ghidra disassembly",
        "reference": "docs/SIGNATURE_CATALOG.md §4.1",
        "notes": "Distinctive: 51 8B 0D (push ecx; mov ecx,[imm32]) + IMUL 0x129 stride"
      },
      "disassemblyContext": [
        "PUSH EBP",
        "MOV EBP, ESP",
        "PUSH -1",
        "PUSH 0x101EC887       ; SEH handler (wildcarded)",
        "MOV EAX, FS:[0]",
        "PUSH EAX",
        "MOV FS:[0], ESP",
        "PUSH ECX",
        "MOV ECX, [0x1023cea0] ; error array ptr (wildcarded)"
      ]
    },
    {
      "name": "FUN_10011230_persistence_patch",
      "targetType": "patch_site",
      "callingConvention": "fastcall",
      "expectedRva": "0x11230",
      "hexPattern": "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 81 EC 68 01 00 00 56 57 8D 45 EC 89 4D E8 8B FA C7 45 EC 00 00 00 00 50 57 8D 8D 8C FE FF FF E8 ?? ?? ?? ?? C7 45 FC 00 00 00 00 68 B1 00 00 00",
      "length": 72,
      "expectedMatchCount": 1,
      "patch": {
        "offset": 0,
        "originalBytes": "55 8B EC 6A FF 68 CD D3",
        "replacementBytes": "B8 01 00 00 00 C2 0C 00",
        "originalBytesMask": "FF FF FF FF FF FF 00 00",
        "description": "Patch FUN_10011230 to MOV EAX,1; RET 0x0C — bypasses XmlDocument.Save crash"
      },
      "provenance": {
        "author": "TVS-2.2",
        "date": "2026-08-17",
        "source": "Ghidra + Frida validation",
        "reference": "docs/SIGNATURE_CATALOG.md §4.9"
      }
    },
    {
      "name": "FUN_101d0950_thread_check_INT3",
      "targetType": "patch_site",
      "callingConvention": "thiscall",
      "expectedRva": "0x1D0950",
      "hexPattern": "55 8B EC 56 8B F1 83 BE AE 00 00 00 00 0F 84 ?? ?? ?? ?? FF 75 08 B8 00 20 00 00 66 89 46 1E E8 ?? ?? ?? ??",
      "length": 36,
      "expectedMatchCount": 1,
      "patch": {
        "offset": 67,
        "originalBytes": "CC",
        "replacementBytes": "90",
        "originalBytesMask": "FF",
        "description": "NOP thread-check INT3 in command 0x0F (load program) dispatch helper"
      },
      "containingFunction": "FUN_101d0950",
      "provenance": {
        "author": "TVS-B.1",
        "date": "2026-08-17",
        "source": "Ghidra disassembly",
        "reference": "docs/SIGNATURE_CATALOG.md §4.10"
      }
    }
  ],
  "dataGlobals": [
    {
      "name": "errorArrayPtr",
      "rva": "0x23CEA0",
      "extractionMethod": "instruction_operand",
      "sourceSignature": "StartCompilingProcess",
      "sourceInstructionOffset": 25,
      "notes": "DAT_1023cea0 — global error-array pointer"
    },
    {
      "name": "errorCount",
      "rva": "0x23CEA4",
      "extractionMethod": "instruction_operand",
      "sourceSignature": "StartCompilingProcess",
      "sourceInstructionOffset": 31,
      "notes": "DAT_1023cea4 — global uint32 count of error records"
    },
    {
      "name": "errorHinst",
      "rva": "0x23AAB8",
      "extractionMethod": "instruction_operand",
      "sourceSignature": "_CreateEntryEX@24",
      "sourceInstructionOffset": 108,
      "notes": "DAT_1023aab8 — HINSTANCE for LoadStringA fallback"
    }
  ],
  "exports": [
    {"name": "_OrlibInit@8", "rva": "0x11B4F0", "callingConvention": "stdcall"},
    {"name": "_OrlibExit@0", "rva": "0x11C080", "callingConvention": "stdcall"},
    {"name": "FindExistPos", "rva": "0x1A4230", "callingConvention": "stdcall", "decorated": false},
    {"name": "_CreateEntryEX@24", "rva": "0x159610", "callingConvention": "stdcall"},
    {"name": "_SaveModule_New@20", "rva": "0x159A70", "callingConvention": "stdcall"},
    {"name": "_VerifyModule_New@20", "rva": "0x169630", "callingConvention": "stdcall"},
    {"name": "_CloseWarehouseForCurrentThreadAndPath@4", "rva": "0x1A9940", "callingConvention": "stdcall"}
  ]
}
```

> **Note on `dataGlobals[].rva` (diagnostic-only):** The `rva` field in `dataGlobals` is **diagnostic-only** (used by sigtool for verification and by tests for assertions). At runtime, the resolver extracts data-global RVAs via instruction-operand extraction (the existing `resolveErrorArrayPtrRva()`, `resolveErrorCountRva()`, `resolveErrorHinstRva()` methods). The `extractionMethod`, `sourceSignature`, and `sourceInstructionOffset` fields document the extraction strategy and are the authoritative source for how the resolver locates these globals. This preserves version-robustness — if a data global moves but the referencing function's signature still matches, the resolver finds the new location automatically.

### Schema field reference

| Field | Type | Required | Description |
|---|---|---|---|
| `schemaVersion` | int | Yes | Schema version (currently 1). Bumped on breaking schema changes. |
| `dll.fileName` | string | Yes | DLL file name (`orlib20.dll`). |
| `dll.fileVersion` | string | Yes | PE version resource FileVersion (e.g., `9.50.01.3344`). |
| `dll.sha256` | string | Yes | Uppercase hex SHA-256, 64 chars, no spaces. |
| `dll.peTimestamp` | string | Yes | Hex string COFF TimeDateStamp. |
| `dll.fileSize` | int | Yes | File size in bytes. |
| `dll.textRva` | string | Yes | Hex string .text section RVA. |
| `dll.textVsize` | string | Yes | Hex string .text virtual size. |
| `dll.textRawSize` | string | Yes | Hex string .text raw size. |
| `dll.imageBase` | string | Yes | Hex string image base. |
| `signatures[].name` | string | Yes | Logical target name (matches SIGNATURE_CATALOG.md). |
| `signatures[].targetType` | string | Yes | `call_target` or `patch_site`. |
| `signatures[].callingConvention` | string | Yes | `cdecl`, `fastcall`, `thiscall`, or `stdcall`. |
| `signatures[].expectedRva` | string | Yes | Hex string RVA (diagnostic only; not used for resolution). |
| `signatures[].hexPattern` | string | Yes | Space-separated hex bytes, `??` = wildcard. |
| `signatures[].length` | int | Yes | Number of bytes in the pattern. |
| `signatures[].expectedMatchCount` | int | Yes | Expected unique matches (always 1). |
| `signatures[].patch` | object | Only for `patch_site` | Patch metadata (see below). |
| `signatures[].patch.offset` | int | Yes (patch) | Byte offset from sig match to patch site. |
| `signatures[].patch.originalBytes` | string | Yes (patch) | Hex string original bytes at patch site. |
| `signatures[].patch.replacementBytes` | string | Yes (patch) | Hex string replacement bytes. |
| `signatures[].patch.originalBytesMask` | string | Yes (patch) | Hex string per-byte mask: `FF` = check, `00` = wildcard. |
| `signatures[].patch.description` | string | No | Human-readable patch description. |
| `signatures[].containingFunction` | string | No | For patch sites: name of the containing function. |
| `signatures[].provenance` | object | Yes | Provenance metadata (author, date, source, reference, notes). |
| `signatures[].disassemblyContext` | string[] | No | Surrounding disassembly for manual fallback. |
| `dataGlobals[].name` | string | Yes | Logical name (e.g., `errorArrayPtr`). |
| `dataGlobals[].rva` | string | Yes | Hex string RVA of the data global. |
| `dataGlobals[].extractionMethod` | string | Yes | `instruction_operand` (extracted from referencing instruction). |
| `dataGlobals[].sourceSignature` | string | Yes | Name of the signature whose function references this global. |
| `dataGlobals[].sourceInstructionOffset` | int | Yes | Byte offset within the referencing function. |
| `exports[].name` | string | Yes | Decorated export name (or undecorated for `FindExistPos`). |
| `exports[].rva` | string | Yes | Hex string export RVA. |
| `exports[].callingConvention` | string | Yes | Always `stdcall` for orlib20.dll exports. |
| `exports[].decorated` | bool | No | `false` for undecorated exports (e.g., `FindExistPos`). Default: `true`. |

### 4.2 Version Manifest JSON Schema

`signatures/version_manifest.json`:

```json
{
  "schemaVersion": 1,
  "manifest": [
    {
      "fileVersion": "9.50.01.3344",
      "peTimestamp": "0x5C01735E",
      "fileSize": 4924400,
      "sha256": "318DB24A4F41BDB9FF69E2E647026B788706904CF887CBB0A953E003DB22EC18",
      "textVsize": "0x1F72FC",
      "textRawSize": "0x1F7400",
      "textRva": "0x1000",
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
      "signatureSetFile": "v9.50.01.3344.json",
      "signatureSetId": 1,
      "verified": true,
      "verifiedDate": "2026-08-17",
      "notes": "Baseline version — all 12 signatures verified, all functional tests pass"
    }
  ]
}
```

### Manifest field reference

| Field | Type | Description |
|---|---|---|
| `schemaVersion` | int | Manifest schema version (currently 1). |
| `manifest[]` | array | One entry per known-tested DLL version. |
| `manifest[].fileVersion` | string | Tier 1 primary key. |
| `manifest[].peTimestamp` | string | Tier 2 cross-check (hex string). |
| `manifest[].fileSize` | int | Tier 2 cross-check. |
| `manifest[].sha256` | string | Tier 3 definitive identity. |
| `manifest[].textVsize` | string | .text virtual size (hex string). |
| `manifest[].textRawSize` | string | .text raw size (hex string). |
| `manifest[].textRva` | string | .text RVA (hex string). |
| `manifest[].imageBase` | string | Image base (hex string). |
| `manifest[].exportCount` | int | NumberOfFunctions. |
| `manifest[].exportNamedCount` | int | NumberOfNames. |
| `manifest[].exportHighestOrdinal` | int | Highest ordinal. |
| `manifest[].exportOrdinalBase` | int | Ordinal base. |
| `manifest[].productVersion` | string | PE version resource metadata. |
| `manifest[].productName` | string | PE version resource metadata. |
| `manifest[].fileDescription` | string | PE version resource metadata. |
| `manifest[].companyName` | string | PE version resource metadata. |
| `manifest[].internalName` | string | PE version resource metadata. |
| `manifest[].originalFilename` | string | PE version resource metadata. |
| `manifest[].signatureSetFile` | string | Relative path to the signature set JSON file. |
| `manifest[].signatureSetId` | int | Numeric ID for the signature set (used in generated C++ enum). |
| `manifest[].verified` | bool | Whether this version has been regression-validated. |
| `manifest[].verifiedDate` | string | Date of last verification (ISO format). |
| `manifest[].notes` | string | Human-readable notes. |

### 4.3 Code Generation Pipeline

```
signatures/v9.50.01.3344.json  ─┐
signatures/version_manifest.json ─┤
                                  ▼
                           sigcodegen.py
                                  │
                                  ▼
                    build/generated/signatures.gen.h
                                  │
                                  ▼
                    src/orlib_resolver.cpp
                    (includes signatures.gen.h)
```

`sigcodegen.py` reads the version manifest and all referenced signature set JSON files, then generates a single C++ header (`signatures.gen.h`) containing:

1. **`kOrlibVersionManifest[]`** — a `constexpr` array of `OrlibVersionManifestEntry` (one per manifest entry).
2. **`kSignatureSets[]`** — an array of signature sets, indexed by `SignatureSetId`.
3. **`kSignatures[]`** — the flat array of `OrlibSignatureDef` entries for the active set (backward-compatible with the current `kSignatures[]` consumed by `orlib_resolver.cpp` line 175).
4. **`kDataGlobals[]`** — the 3 data-global RVA definitions.
5. **`kExports[]`** — the 7 export name definitions.
6. **`kSignatureCount`** — the count constant.

### 4.4 Generated Header Structure

The generated `signatures.gen.h` replaces `b1_signatures.h` in the include chain. It must produce data compatible with the existing `SignatureDef` struct (from `b1_signatures.h` lines 29–39) so `orlib_resolver.cpp`'s `buildSignatureCatalog()` function (lines 165–199) requires minimal changes.

**Generated C++ structures:**

```cpp
// =============================================================================
// signatures.gen.h — AUTO-GENERATED by tools/sigcodegen.py
// DO NOT EDIT. Regenerate by rebuilding the project.
//
// Source: signatures/v9.50.01.3344.json
// Manifest: signatures/version_manifest.json
// Generated: <timestamp>
// =============================================================================

#ifndef SIGNATURES_GEN_H
#define SIGNATURES_GEN_H

#include <cstdint>
#include <cstddef>

namespace tsdev {

// ---------------------------------------------------------------------------
// SignatureSetId enum (generated from manifest signatureSetId values)
// OWNED by this generated header — orlib_resolver.h must NOT redefine it.
// Defined before #include "orlib_resolver.h" so that OrlibVersionManifestEntry
// (which contains a SignatureSetId member by value) can use it.
// ---------------------------------------------------------------------------
enum SignatureSetId : uint16_t {
    SIGSET_NONE          = 0,
    SIGSET_V9_50_01_3344 = 1,
    // Future versions added here by sigcodegen.py
};

} // namespace tsdev

#include "orlib_resolver.h"  // Provides OrlibVersionManifestEntry (uses SignatureSetId)

namespace tsdev {

// ---------------------------------------------------------------------------
// Version manifest entry — uses OrlibVersionManifestEntry from orlib_resolver.h
// (included above). No duplicate type definition.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Signature definition (compatible with existing SignatureDef from b1_signatures.h)
// ---------------------------------------------------------------------------
struct SignatureDef {
    const char* name;
    int          rva;
    const char* hexPattern;
    int          expectedMatchCount;
    bool         isPatchSite;
    const char* originalBytes;
    const char* replacementBytes;
    const char* originalBytesMask;  // Per-byte mask: "FF" = check, "00" = wildcard (hex string, e.g. "FF FF FF FF FF FF 00 00")
    int          patchOffset;
    const char* containingFunction;
};

// ---------------------------------------------------------------------------
// Data global definition
// ---------------------------------------------------------------------------
struct DataGlobalDef {
    const char* name;
    int          rva;
    const char* extractionMethod;
    const char* sourceSignature;
    int          sourceInstructionOffset;
};

// ---------------------------------------------------------------------------
// Export definition
// ---------------------------------------------------------------------------
struct ExportDef {
    const char* name;
    int          rva;
    const char* callingConvention;
    bool         decorated;
};

// ---------------------------------------------------------------------------
// Signature set: groups signatures + data globals + exports for one DLL version
// ---------------------------------------------------------------------------
struct SignatureSet {
    SignatureSetId            id;
    const SignatureDef*       signatures;
    int                       signatureCount;
    const DataGlobalDef*      dataGlobals;
    int                       dataGlobalCount;
    const ExportDef*          exports;
    int                       exportCount;
};

// ---------------------------------------------------------------------------
// Generated data (from v9.50.01.3344.json)
// ---------------------------------------------------------------------------

// Signature set 1: v9.50.01.3344
constexpr SignatureDef kSignatures_v9_50_01_3344[] = {
    {"StartCompilingProcess", 0x165DE0,
     "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 "
     "51 8B 0D ?? ?? ?? ?? 33 C0 A3 ?? ?? ?? ?? 85 C9 74 15 51 FF 15 ?? ?? ?? ?? "
     "A3 ?? ?? ?? ?? 85 C0 75 19 A1 ?? ?? ?? ?? 69",
     1, false, nullptr, nullptr, nullptr, 0, nullptr},
    // ... (all 12 signatures generated from JSON)
    {"FUN_10011230_persistence_patch", 0x11230,
     "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 "
     "81 EC 68 01 00 00 56 57 8D 45 EC 89 4D E8 8B FA C7 45 EC 00 00 00 00 "
     "50 57 8D 8D 8C FE FF FF E8 ?? ?? ?? ?? C7 45 FC 00 00 00 00 "
     "68 B1 00 00 00",
     1, true,
     "55 8B EC 6A FF 68 CD D3",
     "B8 01 00 00 00 C2 0C 00",
     "FF FF FF FF FF FF 00 00",
     0, "FUN_10011230"},
    // ... (targets 10-12)
};

constexpr DataGlobalDef kDataGlobals_v9_50_01_3344[] = {
    {"errorArrayPtr",  0x23CEA0, "instruction_operand", "StartCompilingProcess", 25},
    {"errorCount",     0x23CEA4, "instruction_operand", "StartCompilingProcess", 31},
    {"errorHinst",     0x23AAB8, "instruction_operand", "_CreateEntryEX@24",     108},
};

constexpr ExportDef kExports_v9_50_01_3344[] = {
    {"_OrlibInit@8",                          0x11B4F0, "stdcall", true},
    {"_OrlibExit@0",                          0x11C080, "stdcall", true},
    {"FindExistPos",                          0x1A4230, "stdcall", false},
    {"_CreateEntryEX@24",                     0x159610, "stdcall", true},
    {"_SaveModule_New@20",                    0x159A70, "stdcall", true},
    {"_VerifyModule_New@20",                  0x169630, "stdcall", true},
    {"_CloseWarehouseForCurrentThreadAndPath@4", 0x1A9940, "stdcall", true},
};

constexpr SignatureSet kSignatureSets[] = {
    {SIGSET_V9_50_01_3344,
     kSignatures_v9_50_01_3344, 12,
     kDataGlobals_v9_50_01_3344, 3,
     kExports_v9_50_01_3344, 7},
};

constexpr int kSignatureSetCount =
    static_cast<int>(sizeof(kSignatureSets) / sizeof(kSignatureSets[0]));

// ---------------------------------------------------------------------------
// Backward-compatible aliases (for orlib_resolver.cpp's existing buildSignatureCatalog)
// ---------------------------------------------------------------------------
// The active signature set (first/only set for now; runtime selection added later).
constexpr const SignatureDef* kSignatures = kSignatures_v9_50_01_3344;
constexpr int kSignatureCount = 12;
constexpr const DataGlobalDef* kDataGlobals = kDataGlobals_v9_50_01_3344;
constexpr int kDataGlobalCount = 3;
constexpr const ExportDef* kExports = kExports_v9_50_01_3344;
constexpr int kExportCount = 7;
// Per-version count aliases (referenced by tests, e.g. test_no_hardcoded_rvas.cpp)
constexpr int kDataGlobalCount_v9_50_01_3344 = 3;
constexpr int kExportCount_v9_50_01_3344 = 7;

// ---------------------------------------------------------------------------
// Version manifest (from version_manifest.json)
// Uses OrlibVersionManifestEntry from orlib_resolver.h (included above).
// ---------------------------------------------------------------------------
constexpr OrlibVersionManifestEntry kOrlibVersionManifest[] = {
    {
        "9.50.01.3344",
        0x5C01735E, 4924400,
        "318DB24A4F41BDB9FF69E2E647026B788706904CF887CBB0A953E003DB22EC18",
        0x001F72FC, 0x001F7400, 0x00001000, 0x10000000,
        699, 636, 708, 10,
        "9.50.01.3344", "EasyLanguage", "Omega EasyLanguage",
        "TradeStation Technologies, Inc.", "OR_LIB", "OR_LIB.DLL",
        SIGSET_V9_50_01_3344
    },
};

constexpr size_t kOrlibVersionManifestCount =
    sizeof(kOrlibVersionManifest) / sizeof(kOrlibVersionManifest[0]);

} // namespace tsdev

#endif // SIGNATURES_GEN_H
```

> **Note on `constexpr` for data arrays (C++17):** The generated header uses `constexpr` (not `static const`) for all data arrays (signatures, data globals, exports, manifest). This is valid in C++17 for aggregate-initializable POD types — the arrays are initialized with constant expressions and their addresses are usable in constant-expression contexts.

> **Note on `callingConvention` and `targetType` fields:** The `callingConvention` and `targetType` fields from the JSON schema are used by `sigcodegen.py` for validation and by `sigtool` for diagnostics. They are not emitted to the generated C++ structs because calling conventions are encoded in the resolver's typed function-pointer typedefs (`orlib_resolver.h` lines 373–407).

### 4.5 Integration with orlib_resolver.cpp

**What changes in `orlib_resolver.cpp` / `orlib_resolver.h`:**

1. **Line 34**: `#include "b1_signatures.h"` → `#include "signatures.gen.h"`.
2. **`orlib_resolver.h` — remove `SignatureSetId` enum**: The `SignatureSetId` enum (lines 237–240) must be removed from `orlib_resolver.h`. It is now owned by the generated header, which defines it before including `orlib_resolver.h` so that `OrlibVersionManifestEntry` (which has a `SignatureSetId` member by value) can use it. After removing the enum definition, add a forward declaration `enum SignatureSetId : uint16_t;` in its place. This keeps `orlib_resolver.h` compilable standalone for files that use `SignatureSetId` only by value in struct members or by reference (like `tsdev_lib.cpp`). The forward declaration of a scoped enum with an underlying type is valid C++11+ and allows the enum to be used as a struct member type. Files that use enum *values* directly (like `test_orlib_resolver.cpp`, which references `SIGSET_V9_50_01_3344` and `SIGSET_NONE`) must include `signatures.gen.h` to get the full definition.
3. **`buildSignatureCatalog()` (lines 165–199)**: Updated to propagate the new `originalBytesMask` field. The generated header produces `kSignatures[]` and `kSignatureCount` with the `SignatureDef` struct layout (now including `originalBytesMask` as a raw hex `const char*`). The function reads `kSignatures[i]` and converts to `OrlibSignatureDef` — it must be updated to parse the `originalBytesMask` hex string into a `uint8_t` array (following the same pattern used for `bytes`, `mask`, `originalBytes`, and `replacementBytes`) and propagate it to `OrlibSignatureDef`. An `originalBytesMask` (`const uint8_t*`) and `originalBytesMaskLen` (`int`) field must be added to `OrlibSignatureDef` in `orlib_resolver.h` (lines 215–229). This matches the existing pattern where the generated `SignatureDef` stores raw hex strings (`const char*`) but `OrlibSignatureDef` stores parsed byte arrays (`const uint8_t*`) so the resolver can scan without parsing at runtime.
4. **Version manifest**: The generated `kOrlibVersionManifest[]` replaces the manually-maintained array from `VERSION_DETECTION.md` §6.2. The manifest type is now `OrlibVersionManifestEntry` (from `orlib_resolver.h`, included by the generated header) — no type change is needed in `detectVersion()` or `VersionDescriptor::manifestEntry`. However, the manifest data source changes from a local `constexpr` array defined in `orlib_resolver.cpp` to the generated header's array. The existing `kOrlibVersionManifest[]` and `kOrlibVersionManifestCount` definitions in `orlib_resolver.cpp` (lines 42–74) must be removed to avoid redefinition.
5. **Data globals**: The generated `kDataGlobals[]` array provides the 3 data-global RVAs. The `rva` field in dataGlobals is **diagnostic-only** (used by sigtool for verification and by tests for assertions). At runtime, the resolver extracts data-global RVAs via instruction-operand extraction (the existing `resolveErrorArrayPtrRva()`, `resolveErrorCountRva()`, `resolveErrorHinstRva()` methods). The `extractionMethod`, `sourceSignature`, and `sourceInstructionOffset` fields document the extraction strategy and are the authoritative source for how the resolver locates these globals. This preserves version-robustness — if a data global moves but the referencing function's signature still matches, the resolver finds the new location automatically.

**What stays the same:**

- The `OrlibResolver` class interface (`orlib_resolver.h`) — no public API changes.
- The `OrlibSignatureDef` struct (lines 215–229) — modified: `originalBytesMask` (`const uint8_t*`) and `originalBytesMaskLen` (`int`) fields added to support per-byte masking of patch-site original bytes.
- The `VersionDescriptor`, `PatchSiteDescriptor`, `OrlibResolverError` types — unchanged.
- All resolution logic (PE parsing, signature scanning, export resolution, patch validation/application) — unchanged.
- `tsdev_lib.h` / `tsdev_lib.cpp` — unchanged (the forward declaration of `SignatureSetId` in `orlib_resolver.h` preserves backward compatibility; `tsdev_lib.cpp` uses `VersionDescriptor` by reference, which works with a forward-declared enum type).

### 4.6 Multi-Version Selection at Runtime

The runtime flow for version-aware signature selection:

```
1. OrlibResolver constructor loads the DLL (or in-memory buffer for tests).
2. detectVersion() reads FileVersion from the PE version resource.
3. detectVersion() scans kOrlibVersionManifest[] (generated) for a matching entry.
4. If found: record the SignatureSetId from the manifest entry.
5. If not found: signatureSet = SIGSET_NONE; proceed with the default/first set.
6. The resolver selects kSignatureSets[signatureSetId-1] as the active signature set.
7. All resolve*() methods use the active set's signatures for scanning.
```

Currently, with only one version, `signatureSet()` always returns `SIGSET_V9_50_01_3344` and the active set is always `kSignatureSets[0]`. When a second version is added:

1. A new JSON file (e.g., `v9.50.01.3400.json`) is committed to `signatures/`.
2. A new entry is added to `version_manifest.json` with `signatureSetId: 2`.
3. `sigcodegen.py` generates the new set in `signatures.gen.h` automatically.
4. `detectVersion()` returns `SIGSET_V9_50_01_3400` for the new version.
5. The resolver selects `kSignatureSets[1]` — no code changes needed.

The `OrlibResolver::findSignature()` method (orlib_resolver.h line 921) currently looks up signatures by name in `m_signatures`. In the multi-version design, `m_signatures` is populated from the selected signature set's array at construction time, so `findSignature()` works unchanged.

---

## 5. Tooling Architecture

### 5.1 sigtool — CLI Verifier

`sigtool` is a standalone C++ CLI that wraps the existing scanner logic from `OrlibResolver` for signature verification against a DLL file. It does NOT load the DLL into memory or require a TradeStation install — it reads the DLL from disk and scans the `.text` section, exactly like the current `sig_scan.cpp` probe.

**Source:** `src/sigtool.cpp` (new file, compiled as a separate CMake target).

**Commands:**

```
Usage:
  sigtool verify --dll <path> [--signatures <json>] [--verbose]
    Scans all signatures against <dll>'s .text section.
    Default: uses the committed signature set for the DLL's detected version.
    With --signatures: uses the specified JSON file (for candidate verification).
    Reports per-signature: name, match count, match RVA(s), PASS/FAIL.
    Exit 0 if all pass (unique match), exit 1 if any fail.

  sigtool verify-patch --dll <path> [--signatures <json>]
    Validates patch-site original bytes for all patch_site signatures.
    Exit 0 if all match, exit 1 if any mismatch.

  sigtool info --dll <path>
    Prints DLL version indicators: FileVersion, SHA-256, PE timestamp,
    file size, .text section info, export count.
    Exit 0 on success, exit 1 on read error.

  sigtool scan --dll <path> --pattern "55 8B EC ?? ??"
    Scans a single ad-hoc hex pattern. Reports all match RVAs.
    Exit 0 if at least one match, exit 1 if no match.

  sigtool manifest --dll <path>
    Looks up the DLL's FileVersion in the version manifest.
    Reports: known/unknown, tier match level, signature set ID.
    Exit 0 if known, exit 1 if unknown.
```

**Exit codes:**

| Code | Meaning |
|---|---|
| 0 | All checks passed. |
| 1 | At least one check failed (signature no-match, multi-match, patch mismatch, unknown version). |
| 2 | Error (file not found, invalid PE, invalid JSON, usage error). |

**Implementation approach:**

`sigtool` reuses the PE parsing and signature scanning code from `orlib_resolver.cpp`. The cleanest approach is to extract the PE parsing + scanning logic into a shared static library (`sig_scanner.lib`) that both `orlib_resolver` and `sigtool` link against. Alternatively, `sigtool` can link `orlib_resolver` directly and use the test-mode constructor (`OrlibResolver(image, size, base)`) to scan a DLL read from disk.

**Recommended approach:** Link `sigtool` against `orlib_resolver` and use the in-memory buffer constructor. Read the DLL file into a `std::vector<uint8_t>`, construct `OrlibResolver(image.data(), image.size(), 0x10000000)`, and call `resolveBySignature()` for each signature. This reuses all existing tested code with zero duplication.

**JSON parsing for `--signatures` flag:** `sigtool` needs to parse JSON when a custom signature file is specified. Use a minimal single-header JSON library (e.g., nlohmann/json) included only in `sigtool`'s translation unit. The production CLI (`tsdev_cli`) does NOT depend on JSON — it consumes the generated header. Only `sigtool` (a developer tool, not shipped to end users) has the JSON dependency.

### 5.2 GenerateSignatures.py — Ghidra Headless Script

**Location:** `tools/ghidra_scripts/GenerateSignatures.py`

**Purpose:** Analyze a DLL in Ghidra headless mode, locate the 12 target functions, extract prologue bytes with automatic operand wildcarding, verify uniqueness, and output a candidate signature JSON file.

**Inputs:**
- The DLL file to analyze (passed via `analyzeHeadless -import`).
- A target definitions file (embedded in the script or passed as a script argument) listing the 12 target names, their known RVAs (for baseline), max signature lengths, and patch metadata.

**Outputs:**
- A candidate signature JSON file (schema-compatible with §4.1) written to a path specified as a script argument.

**Auto-wildcarding design** (from SIGNATURE_INFRASTRUCTURE_REPORT.md §2.5, §5.2):

```python
def should_mask_operand(ins, op_index):
    """Returns True if the operand should be wildcarded."""
    op_type = ins.getOperandType(op_index)
    return bool(op_type & OperandType.DYNAMIC) or bool(op_type & OperandType.ADDRESS)

def get_masked_instruction(ins):
    """Returns BytePattern list for one instruction with auto-wildcarding."""
    mask = [0] * ins.length
    proto = ins.getPrototype()
    for op in range(proto.getNumOperands()):
        if should_mask_operand(ins, op):
            value_mask = proto.getOperandValueMask(op)
            mask_bytes = value_mask.getBytes()
            mask = [m | (v & 0xFF) for m, v in zip(mask, mask_bytes)]
    result = []
    for m, b in zip(mask, ins.getBytes()):
        if m == 0xFF:
            result.append(BytePattern(is_wildcard=True, byte=None))
        else:
            result.append(BytePattern(byte=b & 0xFF, is_wildcard=False))
    return result
```

**Wildcard categories handled automatically:**

| Category | OperandType flag | Example instructions |
|---|---|---|
| Relative call/jump targets | `DYNAMIC` | `E8 rel32`, `0F 8x rel32` |
| Absolute addresses (IAT, data refs) | `ADDRESS` | `FF 15 imm32`, `MOV reg,[imm32]`, `MOV [imm32],reg` |
| SEH handler addresses | `ADDRESS` | `68 imm32` (PUSH handler) |

**Kept fixed (NOT wildcarded):**

| Category | Why fixed |
|---|---|
| Opcode bytes | Structural; don't change across rebuilds |
| ModR/M bytes | Register encodings; stable |
| Structural immediates | Stack frame sizes, record sizes, allocation sizes |
| Short jump displacements (1-byte) | Stable unless code is reorganized |

**Uniqueness verification:** The script scans the `.text` section for the generated pattern and counts matches. If `matchCount == 1`, the signature is accepted. If `matchCount > 1`, the script extends the signature by including more instructions and re-scans (iterative extension, up to `maxLen`). If `matchCount == 0`, an error is reported (should not happen for a signature extracted from the same binary).

**Invocation:**

```bash
analyzeHeadless /tmp/ghidra_proj TSDevSigGen \
    -import orlib20_new.dll \
    -postScript GenerateSignatures.py /tmp/candidate_signatures.json \
    -scriptPath tools/ghidra_scripts \
    -deleteProject
```

**Limitations (from report §2.6):**
- Auto-analysis may not identify all internal functions → use known RVAs for baseline, anchor strategies for new versions.
- Jython vs PyGhidra compatibility → use `#@runtime PyGhidra` directive.
- RVA-based lookup only works for the baseline → anchor-based relocation is a future enhancement.

### 5.3 sigcodegen.py — JSON → C++ Header Generator

**Location:** `tools/sigcodegen.py`

**Purpose:** Read the version manifest + all referenced signature set JSON files and generate `signatures.gen.h`.

**CLI interface:**

```
Usage:
  python tools/sigcodegen.py --manifest <version_manifest.json> \
                             --sigdir <signatures_dir> \
                             --output <signatures.gen.h>
```

**Arguments:**

| Flag | Required | Description |
|---|---|---|
| `--manifest` | Yes | Path to `version_manifest.json`. |
| `--sigdir` | Yes | Directory containing signature set JSON files. |
| `--output` | Yes | Path to write the generated header. |

**Logic:**

1. Parse `version_manifest.json` → list of manifest entries.
2. For each manifest entry, load the referenced `signatureSetFile` from `--sigdir`.
3. Validate each signature set: 12 signatures, 3 data globals, 7 exports, all required fields present.
4. Generate C++ code:
   - `SignatureSetId` enum (from `signatureSetId` values).
   - Per-version `kSignatures_<version>[]` arrays (from JSON `signatures[]`).
   - Per-version `kDataGlobals_<version>[]` arrays (from JSON `dataGlobals[]`).
   - Per-version `kExports_<version>[]` arrays (from JSON `exports[]`).
   - `kSignatureSets[]` array grouping per-version data.
   - `kOrlibVersionManifest[]` array (from manifest entries).
   - Backward-compatible `kSignatures` / `kSignatureCount`, `kDataGlobals` / `kDataGlobalCount`, `kExports` / `kExportCount` aliases (pointing to the first set), plus per-version count aliases (`kDataGlobalCount_<version>`, `kExportCount_<version>`).
5. Write the generated header to `--output`.

**Validation rules (fail generation on violation):**
- Every manifest entry's `signatureSetFile` must exist in `--sigdir`.
- Every signature set must have exactly 12 signatures.
- Every signature set must have exactly 3 data globals.
- Every signature set must have exactly 7 exports.
- `hexPattern` must be valid hex with `??` wildcards only.
- `length` must match the actual byte count in `hexPattern`.
- `expectedMatchCount` must be 1.
- `signatureSetId` values must be unique across manifest entries.
- No duplicate signature names within a set.

### 5.4 sigdiff.py — Signature Diff Tool

**Location:** `tools/sigdiff.py`

**Purpose:** Compare two signature set JSON files and report which signatures changed, which are stable, and what specifically changed (hex pattern, wildcards, length, patch metadata).

**CLI interface:**

```
Usage:
  python tools/sigdiff.py --old <old.json> --new <new.json> [--verbose]
```

**Output format:**

```
Signature Diff: v9.50.01.3344.json → v9.50.01.3400.json
=========================================================

STABLE (10):
  StartCompilingProcess      — identical
  EndCompilingProcess        — identical
  ...

CHANGED (2):
  FUN_101d0950_thread_check_INT3
    hexPattern: CHANGED (3 bytes differ at offsets 15-17)
    length:     36 → 39 (extended by 3 bytes)
    wildcards:  4 → 5 (1 new wildcard at offset 17)
    patch:      unchanged (offset=0x43, CC→90)

  FUN_101d1fa0_thread_check_INT3
    hexPattern: CHANGED (wildcard at offset 22 changed from ?? to 8B)
    length:     unchanged (57)
    patch:      unchanged (offset=0x28, CC→90)

NEW (0):
REMOVED (0):

Data Globals:
  errorArrayPtr  — STABLE (RVA 0x23CEA0)
  errorCount     — STABLE (RVA 0x23CEA4)
  errorHinst     — CHANGED (RVA 0x23AAB8 → 0x23ABC0)

Exports:
  All 7 exports: STABLE
```

**Exit codes:**

| Code | Meaning |
|---|---|
| 0 | All signatures stable (no changes). |
| 1 | At least one signature changed (review needed). |
| 2 | Error (file not found, invalid JSON). |

### 5.5 Tool Integration in the Update Workflow

The tools fit into the update workflow (from SIGNATURE_INFRASTRUCTURE_REPORT.md §3.1) as follows:

```
1. RECEIVE new DLL
   └─ sigtool info --dll new.dll          → get version indicators
   [AUTOMATED]

2. TRY EXISTING SIGNATURES
   └─ sigtool verify --dll new.dll         → scan existing sigs against new DLL
   └─ sigtool verify-patch --dll new.dll   → check patch-site original bytes
   [AUTOMATED — 5 seconds]

3. DECISION GATE
   ├─ ALL PASS → go to step 4
   └─ SOME FAIL → go to step 5
   [AUTOMATED]

4. ALL MATCHED
   └─ Run functional regression tests (test_insert, test_delete, test_modify)
   └─ Add manifest entry (sigtool info output → paste into version_manifest.json)
   └─ Rebuild (sigcodegen.py runs automatically via CMake)
   └─ DONE
   [AUTOMATED except manifest edit — 2 minutes]

5. SOME FAILED — REGENERATE
   └─ analyzeHeadless + GenerateSignatures.py  → candidate_signatures.json
   └─ sigdiff.py --old current.json --new candidate_signatures.json
   └─ Human reviews diff (~15-30 min)
   └─ sigtool verify --dll new.dll --signatures candidate_signatures.json
   └─ Run functional regression tests
   └─ Copy candidate JSON to signatures/v<version>.json
   └─ Add manifest entry
   └─ Rebuild + commit
   [SEMI-AUTOMATED — 15-30 min human review]
```

---

## 6. Build System Design

### 6.1 Target CMakeLists.txt Structure

The root `CMakeLists.txt` is restructured to separate production targets from tests, integrate the codegen step, and remove all probe targets.

```cmake
# CMakeLists.txt -- tsdev_automation
#
# ALL builds target Win32 (x86 / 32-bit) because the project loads 32-bit
# orlib20.dll from the TradeStation 9.5 install.
#
# Configure:
#   cmake -S . -B build -A Win32
#   cmake --build build --config Release

cmake_minimum_required(VERSION 3.20)
project(tsdev_automation LANGUAGES CXX)

# C++17 standard, no compiler extensions.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Guard: refuse to configure for anything other than 32-bit (Win32).
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "tsdev_automation MUST be built as 32-bit (x86) to load orlib20.dll. "
        "Configure with -A Win32. "
        "Detected sizeof(void*)=${CMAKE_SIZEOF_VOID_P}.")
endif()

# --- Find Python for codegen -------------------------------------------------
find_program(PYTHON_EXECUTABLE python REQUIRED)

# --- Signature codegen: JSON → signatures.gen.h ------------------------------
set(SIG_MANIFEST ${CMAKE_SOURCE_DIR}/signatures/version_manifest.json)
set(SIG_DIR ${CMAKE_SOURCE_DIR}/signatures)
set(GEN_HEADER ${CMAKE_BINARY_DIR}/generated/signatures.gen.h)
set(SIGCODEGEN ${CMAKE_SOURCE_DIR}/tools/sigcodegen.py)

# Collect all JSON files in signatures/ as codegen dependencies
# CONFIGURE_DEPENDS ensures new JSON files trigger reconfiguration (requires CMake 3.12+,
# satisfied by the 3.20 minimum above).
file(GLOB SIG_JSON_FILES CONFIGURE_DEPENDS "${SIG_DIR}/*.json")

add_custom_command(
    OUTPUT ${GEN_HEADER}
    COMMAND ${PYTHON_EXECUTABLE} ${SIGCODEGEN}
            --manifest ${SIG_MANIFEST}
            --sigdir ${SIG_DIR}
            --output ${GEN_HEADER}
    DEPENDS ${SIG_JSON_FILES} ${SIGCODEGEN} ${SIG_MANIFEST}
    COMMENT "Generating signature header from JSON (sigcodegen.py)"
    VERBATIM
)
add_custom_target(siggen DEPENDS ${GEN_HEADER})

# --- Generated header include directory --------------------------------------
set(GEN_INCLUDE_DIR ${CMAKE_BINARY_DIR}/generated)

# --- orlib_resolver library (production) -------------------------------------
add_library(orlib_resolver STATIC src/orlib_resolver.cpp)
target_include_directories(orlib_resolver PUBLIC src)
target_include_directories(orlib_resolver PRIVATE ${GEN_INCLUDE_DIR})
target_link_libraries(orlib_resolver PUBLIC bcrypt)
add_dependencies(orlib_resolver siggen)

# --- tsdev_lib library (production) ------------------------------------------
add_library(tsdev_lib STATIC src/tsdev_lib.cpp)
target_include_directories(tsdev_lib PUBLIC src)
target_link_libraries(tsdev_lib PUBLIC orlib_resolver)

# --- Production CLI ----------------------------------------------------------
add_executable(tsdev_cli src/tsdev_cli.cpp)
target_link_libraries(tsdev_cli tsdev_lib)

# --- sigtool (developer tool, not shipped) -----------------------------------
add_executable(sigtool src/sigtool.cpp)
target_link_libraries(sigtool orlib_resolver)
target_include_directories(sigtool PRIVATE ${GEN_INCLUDE_DIR})
add_dependencies(sigtool siggen)

# --- nlohmann/json for sigtool (vendored single-header) ----------------------
# The file tools/third_party/nlohmann/json.hpp must be vendored (downloaded and
# committed to the repo). Use a stable release at least 7 days old (e.g., 3.11.3
# or later). Only sigtool depends on JSON; the production CLI has no JSON dependency.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE tools/third_party/nlohmann)
target_link_libraries(sigtool PRIVATE nlohmann_json)

# === TESTS ===================================================================
# Tests are built but not run automatically. Run manually:
#   build\Release\test_insert.exe
#   build\Release\test_orlib_resolver.exe
#   etc.

enable_testing()

# --- Test: OrlibResolver unit tests ------------------------------------------
add_executable(test_orlib_resolver tests/test_orlib_resolver.cpp)
target_link_libraries(test_orlib_resolver PRIVATE orlib_resolver)
target_include_directories(test_orlib_resolver PRIVATE ${GEN_INCLUDE_DIR})
add_dependencies(test_orlib_resolver siggen)
add_test(NAME orlib_resolver COMMAND test_orlib_resolver)

# --- Test: INSERT functional regression --------------------------------------
add_executable(test_insert tests/test_insert.cpp)
target_link_libraries(test_insert tsdev_lib)
add_test(NAME insert COMMAND test_insert)

# --- Test: DELETE functional regression --------------------------------------
add_executable(test_delete tests/test_delete.cpp)
target_link_libraries(test_delete tsdev_lib)
add_test(NAME delete COMMAND test_delete)

# --- Test: DELETE persistence check ------------------------------------------
add_executable(test_delete_check tests/test_delete_check.cpp)
target_link_libraries(test_delete_check tsdev_lib)

# --- Test: DELETE diagnostic -------------------------------------------------
add_executable(test_delete_diag tests/test_delete_diag.cpp)
target_link_libraries(test_delete_diag tsdev_lib)

# --- Test: MODIFY functional regression --------------------------------------
add_executable(test_modify tests/test_modify.cpp)
target_link_libraries(test_modify tsdev_lib)
add_test(NAME modify COMMAND test_modify)

# --- Test: Persistence patch byte validation ---------------------------------
add_executable(test_persistence_validation tests/test_persistence_validation.cpp)
target_link_libraries(test_persistence_validation tsdev_lib)

# --- Test: No hardcoded RVA constants guard ----------------------------------
# Derives its forbidden-constants list from the generated header (TVS-E.4).
# No library linked (arrays are constexpr/header-only), but compiles against
# signatures.gen.h and depends on the siggen target.
add_executable(test_no_hardcoded_rvas tests/test_no_hardcoded_rvas.cpp)
target_include_directories(test_no_hardcoded_rvas PRIVATE ${GEN_INCLUDE_DIR})
add_dependencies(test_no_hardcoded_rvas siggen)
add_test(NAME no_hardcoded_rvas COMMAND test_no_hardcoded_rvas)
```

### 6.2 Codegen Integration Details

The codegen step uses CMake's `add_custom_command` → `add_custom_target` → `add_dependencies` pattern:

1. **`add_custom_command(OUTPUT ${GEN_HEADER})`** — declares how to generate `signatures.gen.h`. The `DEPENDS` list includes all JSON files in `signatures/` and the `sigcodegen.py` script itself, so any change to a JSON file or the script triggers regeneration.

2. **`add_custom_target(siggen DEPENDS ${GEN_HEADER})`** — creates a top-level target that depends on the generated header.

3. **`add_dependencies(orlib_resolver siggen)`** — ensures `siggen` runs before `orlib_resolver` compiles. Same for `sigtool` and `test_orlib_resolver`.

4. **`target_include_directories(orlib_resolver PRIVATE ${GEN_INCLUDE_DIR})`** — adds `build/generated/` to the include path so `#include "signatures.gen.h"` resolves.

**Key property:** The generated header is in `${CMAKE_BINARY_DIR}/generated/`, which is build output. It is never committed to git. A fresh clone + configure + build regenerates it from the committed JSON files.

### 6.3 What Gets Generated vs What Is Committed

| Artifact | Committed? | Location | Purpose |
|---|---|---|---|
| `signatures/*.json` | **Yes** | `signatures/` | Source of truth for signature data |
| `signatures/version_manifest.json` | **Yes** | `signatures/` | Version → signature set mapping |
| `tools/sigcodegen.py` | **Yes** | `tools/` | Code generator script |
| `tools/sigdiff.py` | **Yes** | `tools/` | Diff tool |
| `tools/ghidra_scripts/*.py` | **Yes** | `tools/ghidra_scripts/` | Ghidra scripts |
| `build/generated/signatures.gen.h` | **No** | `build/generated/` | Generated C++ header (build artifact) |

### 6.4 How Tests Are Built and Run

Tests are built as standard CMake executables. They are NOT run automatically during `cmake --build` — they require a live TradeStation install (for functional tests) or a baseline DLL copy (for resolver tests). The `enable_testing()` / `add_test()` declarations allow running specific tests via `ctest --test-dir build -R <name>` when the environment supports it.

**Test dependencies on codegen:** `test_orlib_resolver` links `orlib_resolver` and includes the generated header directory, so it gets the generated signatures. `test_no_hardcoded_rvas` does NOT link any library, but since TVS-E.4 it DOES compile against the generated header (`signatures.gen.h`) and depends on the `siggen` target — it derives its forbidden-constants list from the generated arrays (see §7.4). It still reads `src/tsdev_lib.cpp` from disk to check for forbidden RVA hex strings (see §7.3).

---

## 7. RVA Deduplication Strategy

### 7.1 Single Source of Truth

In the target architecture, all 15 RVAs (12 code + 3 data) live in exactly one place: the JSON signature set file (`signatures/v9.50.01.3344.json`). From there, they flow to all consumers via code generation:

```
signatures/v9.50.01.3344.json  (SINGLE SOURCE OF TRUTH)
        │
        ▼ sigcodegen.py
        │
build/generated/signatures.gen.h
        │
        ├──► orlib_resolver.cpp  (kSignatures[], kDataGlobals[], kExports[])
        ├──► test_orlib_resolver.cpp  (includes signatures.gen.h for expected RVAs)
        └──► test_no_hardcoded_rvas.cpp  (reads generated header for forbidden constants)
```

### 7.2 RVA Location Map (Target Architecture)

| RVA | Name | Lives in JSON | Consumed via generated header |
|---|---|---|---|
| 0x165DE0 | StartCompilingProcess | `signatures[].expectedRva` | `kSignatures[0].rva` |
| 0x165ED0 | EndCompilingProcess | `signatures[].expectedRva` | `kSignatures[1].rva` |
| 0x165A00 | CELCompiler_ctor | `signatures[].expectedRva` | `kSignatures[2].rva` |
| 0x165AC0 | CELCompiler_dtor | `signatures[].expectedRva` | `kSignatures[3].rva` |
| 0x1A9460 | InitGlobalLookup | `signatures[].expectedRva` | `kSignatures[4].rva` |
| 0x1E4030 | DeleteFromTree | `signatures[].expectedRva` | `kSignatures[5].rva` |
| 0x1A9650 | GetWarehouse | `signatures[].expectedRva` | `kSignatures[6].rva` |
| 0x1A8360 | FUN_101a8360_delete_orchestrator | `signatures[].expectedRva` | `kSignatures[7].rva` |
| 0x11230 | FUN_10011230_persistence_patch | `signatures[].expectedRva` | `kSignatures[8].rva` |
| 0x1D0950 | FUN_101d0950 (containing fn) | `signatures[].expectedRva` | `kSignatures[9].rva` |
| 0x1CF5B0 | FUN_101cf5b0 (containing fn) | `signatures[].expectedRva` | `kSignatures[10].rva` |
| 0x1D1FA0 | FUN_101d1fa0 (containing fn) | `signatures[].expectedRva` | `kSignatures[11].rva` |
| 0x23CEA0 | errorArrayPtr | `dataGlobals[].rva` | `kDataGlobals[0].rva` |
| 0x23CEA4 | errorCount | `dataGlobals[].rva` | `kDataGlobals[1].rva` |
| 0x23AAB8 | errorHinst | `dataGlobals[].rva` | `kDataGlobals[2].rva` |

### 7.3 How Test Assertions Reference RVAs

**Current state:** `test_orlib_resolver.cpp` hardcodes RVAs in `CHECK` macros (e.g., line 198: `"resolveStartCompilingProcess RVA==0x165DE0"`). These are duplicated from `b1_signatures.h`.

**Target state:** `test_orlib_resolver.cpp` includes `signatures.gen.h` and references the generated constants:

```cpp
#include "signatures.gen.h"

// ...
// --- Target 1: StartCompilingProcess ---
{
    auto fn = resolver.resolveStartCompilingProcess();
    uintptr_t rva = (uintptr_t)fn - resolver.baseAddress();
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "resolveStartCompilingProcess RVA==0x%llX (expected 0x%X)",
                  (unsigned long long)rva, tsdev::kSignatures[0].rva);
    CHECK(rva == (uintptr_t)tsdev::kSignatures[0].rva, buf);
}
```

This eliminates duplication: the expected RVA comes from the generated header, which comes from the JSON, which is the single source of truth. If a signature's RVA changes in the JSON, the test assertion updates automatically on the next build.

For patch-site tests, the expected patch RVA is computed from the generated data:

```cpp
// Patch site: FUN_10011230_persistence_patch
{
    auto desc = resolver.resolvePersistencePatch();
    uintptr_t expectedPatchRva = (uintptr_t)tsdev::kSignatures[8].rva  // sig match RVA
                               + tsdev::kSignatures[8].patchOffset;     // + offset
    CHECK(desc.patchRva == expectedPatchRva, "...");
}
```

### 7.4 How test_no_hardcoded_rvas.cpp Evolves

**Status: Option A IMPLEMENTED in TVS-E.4.** The test now derives its forbidden-constants list from `signatures.gen.h` (the generated header). The hardcoded `kForbidden[]` array has been removed.

**Purpose:** Verify that `tsdev_lib.cpp` does not contain hardcoded RVA constants. In the target architecture, the RVAs live in JSON → generated header → `orlib_resolver.cpp`. They should never appear in `tsdev_lib.cpp`.

**Implementation (Option A — implemented):** The test includes `signatures.gen.h` and builds its forbidden-constants list from the generated arrays:

```cpp
#include "signatures.gen.h"

// Build forbidden constants from the generated signature catalog.
// tsdev_lib.cpp must NOT contain any of these RVA hex strings.
std::vector<std::pair<std::string, std::string>> buildForbidden() {
    std::vector<std::pair<std::string, std::string>> result;
    for (int i = 0; i < tsdev::kSignatureCount; i++) {
        const tsdev::SignatureDef& sig = tsdev::kSignatures[i];
        // Patch-site entries: forbid the actual patch address (rva + patchOffset),
        // NOT the signature match-point rva. Non-patch entries: forbid rva directly.
        unsigned int value = sig.isPatchSite
            ? static_cast<unsigned int>(sig.rva + sig.patchOffset)
            : static_cast<unsigned int>(sig.rva);
        char hex[16];
        std::snprintf(hex, sizeof(hex), "0x%x", value);
        result.push_back({hex, sig.name});
    }
    for (int i = 0; i < tsdev::kDataGlobalCount_v9_50_01_3344; i++) {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "0x%x", tsdev::kDataGlobals_v9_50_01_3344[i].rva);
        result.push_back({hex, tsdev::kDataGlobals_v9_50_01_3344[i].name});
    }
    return result;
}
```

**Critical correctness note (patch sites):** For patch-site signature entries (`isPatchSite == true`), `kSignatures[i].rva` is the signature MATCH-POINT RVA, not the patch address. The actual patch-site RVA = `kSignatures[i].rva + kSignatures[i].patchOffset`. The forbidden list must contain the patch-site RVA (e.g. `0x11230`, `0x1d0993`, `0x1cf5d4`, `0x1d1fc8`), so `buildForbidden()` branches on `isPatchSite`. An earlier draft of this section's snippet iterated only `kSignatures[i].rva` directly, which would have missed the 4 patch-site RVAs — this was caught and corrected during TVS-E.4 implementation.

**Exports excluded:** Export RVAs are resolved by name via `GetProcAddress` and were never hardcoded as RVAs in `tsdev_lib.cpp`, so they are intentionally NOT in the forbidden list.

This eliminates the hardcoded `kForbidden[]` array. The forbidden constants are derived from the generated header, which is derived from the JSON source of truth. A TDD coupling proof (bumping a JSON `expectedRva` and observing the test's pass/fail behavior change) confirmed the derivation is genuine.

**Option B (not adopted):** Keeping the hardcoded `kForbidden[]` array with a mirror-comment (`// These constants mirror signatures/v9.50.01.3344.json. Update both when RVAs change.`) was considered as a simpler, less-DRY transitional alternative, but was not adopted — Option A was implemented directly in TVS-E.4.

### 7.5 Resolving Stale Comments

**`orlib_resolver.h` (lines 554, 559, 564, 569, 574, 579, 590, 600, 748–778):** These Doxygen comments mention specific RVAs (e.g., "Resolve Target 1: StartCompilingProcess (RVA 0x165DE0, __cdecl)"). In the target architecture, these comments are updated to reference the JSON source of truth instead of hardcoding the RVA:

```cpp
/// \brief Resolve Target 1: StartCompilingProcess (__cdecl).
/// \return Typed function pointer (StartCompilingProcessFn).
/// \throws OrlibResolverError if the signature does not match uniquely.
/// \see signatures/v9.50.01.3344.json (expectedRva field)
StartCompilingProcessFn resolveStartCompilingProcess();
```

The RVA is removed from the comment and replaced with a `\see` reference to the JSON file. This eliminates the comment-level duplication.

**`tsdev_lib.h`:** Historical comments referencing RVAs are similarly cleaned. The `tsdev_lib.h` header should not mention any specific RVA — it consumes the resolver's typed API and is fully decoupled from version-specific data.

**`tsdev_lib.cpp`:** Already decoupled (uses `OrlibResolver`). Any remaining RVA references in comments are removed.

---

## 8. Probe Archival Strategy

### 8.1 What Gets Archived

The following 26 files are moved from `src/` to `archive/`:

| File | Purpose (historical) | Why archived |
|---|---|---|
| `tsdev_probe.cpp` | Initial headless init smoke test | Superseded by production code |
| `live_probe.cpp` | Live runtime probe | Experimental, not production |
| `elevated_test.cpp` through `elevated_test21.cpp` (21 source files) | Progressive probes for INSERT/DELETE/MODIFY | Superseded by `tsdev_lib.cpp` + tests. CMake also defines an `elevated_test22` target that compiles `elevated_test21.cpp` (a duplicate); this target is removed in TVS-5. |
| `hook_dll.cpp` | DLL injection experiment | Not used in production |
| `inject.cpp` | Injection launcher | Not used in production |
| `sig_scan.cpp` | Standalone uniqueness scanner | Superseded by `sigtool` |

### 8.2 Directory Structure

```
archive/
├── README.md                       # Explains what these are and why archived
├── tsdev_probe.cpp                 # Initial smoke test
├── live_probe.cpp                  # Live runtime probe
├── elevated_test.cpp               # First elevated probe
├── elevated_test2.cpp              # ...
├── ... (elevated_test3 through elevated_test20)
├── elevated_test21.cpp             # Last elevated probe
├── hook_dll.cpp                    # DLL injection experiment
├── inject.cpp                      # Injection launcher
└── sig_scan.cpp                    # Standalone uniqueness scanner (superseded by sigtool)
```

### 8.3 Removal from CMake Build

All probe targets are removed from `CMakeLists.txt`. The current lines 31–57 (all `add_executable` for probes, `add_library(hook_dll ...)`, `add_executable(inject ...)`, `add_executable(sig_scan ...)`) are deleted. The `sig_scan` target's include directory (`target_include_directories(sig_scan PRIVATE tmp/re_artifacts)`, line 105) is also removed.

After removal, the CMake build produces only:
- **Libraries:** `orlib_resolver`, `tsdev_lib`
- **Executables:** `tsdev_cli`, `sigtool`
- **Tests:** `test_orlib_resolver`, `test_insert`, `test_delete`, `test_delete_check`, `test_delete_diag`, `test_modify`, `test_persistence_validation`, `test_no_hardcoded_rvas`

### 8.4 Reference Value and Documentation

The archived probes retain reference value:

1. **Historical context:** They document the incremental RE process that discovered the orlib20 function parameters, patch sites, and calling conventions.
2. **Debugging reference:** If a regression occurs, the probes show the exact sequence of calls that was validated during development.
3. **`sig_scan.cpp`** is the direct predecessor of `sigtool` — its PE parsing + signature scanning logic is the basis for the `sigtool` implementation.

**`archive/README.md` content:**

```markdown
# Archived Experimental Probes

These files are historical experimental probes used during the reverse-engineering
of orlib20.dll. They are NOT compiled and NOT part of the production build.

## Why they're here

During development, 21 progressive probes (elevated_test*.cpp) were created to
validate parameter orders, patch sites, and calling conventions one step at a time.
Each probe tested a specific hypothesis. Once the findings were integrated into
the production code (src/tsdev_lib.cpp, src/orlib_resolver.cpp) and the functional
test suite (tests/test_*.cpp), the probes became obsolete.

## How to reference them

- `tsdev_probe.cpp` — Initial headless init smoke test (TVS-2.1).
- `elevated_test20.cpp` — The probe that demonstrated standalone INSERT persistence
  with the FUN_10011230 patch. This is the reference implementation cited in
  tsdev_lib.h.
- `sig_scan.cpp` — Standalone signature uniqueness scanner. The PE parsing and
  scanning logic here is the basis for src/sigtool.cpp.
- `hook_dll.cpp` / `inject.cpp` — DLL injection experiments (retired approach).

## Why they're not deleted

They document the RE methodology and serve as debugging references if a regression
occurs in the production code. They are not compiled to keep the build clean.
```

---

## 9. Migration Constraints

### 9.1 Principles

1. **Every TVS leaves the project in a buildable, testable state.** After each TVS, `cmake --build build --config Release` succeeds and the CLI (`tsdev_cli insert/delete/modify`) works end-to-end.
2. **No big-bang rewrites.** Each TVS makes one focused change. The migration is decomposed into 7 TVSs (below).
3. **TDD red-green for each TVS.** Write the failing test first, then implement until the test passes.
4. **The CLI must work end-to-end after each TVS.** The insert/delete/modify commands must function correctly after every step.

### 9.2 TVS Sequence

#### TVS-1: Create JSON signature data + sigcodegen.py (build dependency fix)

**Goal:** Move signature data from `tmp/re_artifacts/b1_signatures.h` to committed JSON, generate the C++ header at build time.

**Steps:**
1. Create `signatures/v9.50.01.3344.json` by transcribing all 12 signatures, 3 data globals, and 7 exports from `b1_signatures.h` + `SIGNATURE_CATALOG.md` into JSON.
2. Create `signatures/version_manifest.json` with the single baseline entry.
3. Write `tools/sigcodegen.py` that reads the JSON and generates `signatures.gen.h`.
4. Update `CMakeLists.txt`: add the codegen custom command, add `siggen` target, add `add_dependencies(orlib_resolver siggen)`, add `${GEN_INCLUDE_DIR}` to `orlib_resolver` include dirs.
5. Update `src/orlib_resolver.cpp` line 34: `#include "b1_signatures.h"` → `#include "signatures.gen.h"`.
6. Remove the `SignatureSetId` enum definition from `src/orlib_resolver.h` (lines 237–240) and replace it with a forward declaration: `enum SignatureSetId : uint16_t;` — the full definition is now in `signatures.gen.h`.
7. Add `#include "signatures.gen.h"` at the top of `src/test_orlib_resolver.cpp` (before `#include "orlib_resolver.h"`) — this test uses `SignatureSetId::SIGSET_V9_50_01_3344` and `SignatureSetId::SIGSET_NONE` enum values directly, which require the full definition from the generated header. The forward declaration in `orlib_resolver.h` is insufficient for enum value access.
8. Remove the `kOrlibVersionManifest[]` and `kOrlibVersionManifestCount` definitions from `src/orlib_resolver.cpp` (lines 42–74) — these are now provided by the generated header.
9. Add an `originalBytesMask` (`const uint8_t*`) and `originalBytesMaskLen` (`int`) field to `OrlibSignatureDef` in `src/orlib_resolver.h`. Update `buildSignatureCatalog()` in `src/orlib_resolver.cpp` to parse the `originalBytesMask` hex string (from the generated `SignatureDef`'s `const char*` field) into a `uint8_t` array and propagate it to `OrlibSignatureDef`.
10. Remove `target_include_directories(orlib_resolver PRIVATE tmp/re_artifacts)` from CMakeLists.txt (line 112).
11. Remove `target_include_directories(sig_scan PRIVATE tmp/re_artifacts)` (line 105) — but `sig_scan` is being archived, so this goes away in TVS-5.
12. **TDD red:** Fresh clone (without `tmp/`) → build fails (no `b1_signatures.h`). **TDD green:** After JSON + codegen → build succeeds without `tmp/`.
13. **Regression:** Run `test_orlib_resolver`, `test_insert`, `test_delete`, `test_modify` — all must pass.
14. **Adversarial review:** Verify the generated header is byte-identical in data to `b1_signatures.h` (same hex patterns, same RVAs, same patch metadata).

**After TVS-1:** Fresh clone builds. `b1_signatures.h` is no longer a build dependency. RVAs still duplicated in tests and comments (addressed in later TVSs).

#### TVS-2: Create sigtool CLI

**Goal:** Provide the "try existing first" verification tool.

**Steps:**
1. Write `src/sigtool.cpp` implementing `verify`, `verify-patch`, `info`, `scan`, `manifest` commands.
2. Link `sigtool` against `orlib_resolver` (uses in-memory buffer constructor for disk-based scanning).
3. Add `sigtool` target to `CMakeLists.txt`.
4. **TDD red:** `sigtool verify --dll tests/fixtures/orlib20_baseline.dll` doesn't exist. **TDD green:** All 12 signatures report PASS.
5. **Regression:** Existing tests still pass.

**After TVS-2:** `sigtool` is available for the update workflow. No changes to production code.

#### TVS-3: Deduplicate test_orlib_resolver.cpp RVAs

**Goal:** Test assertions reference RVAs from the generated header, not hardcoded constants.

**Steps:**
1. `#include "signatures.gen.h"` was already added to `test_orlib_resolver.cpp` in TVS-1 step 7 (needed for `SignatureSetId` enum value access) — this step is now a no-op / already satisfied.
2. Replace each hardcoded RVA in `CHECK` macros with `tsdev::kSignatures[i].rva`.
3. For patch-site tests, compute expected patch RVA from `kSignatures[i].rva + kSignatures[i].patchOffset`.
4. For export tests, use `tsdev::kExports_v9_50_01_3344[i].rva`.
5. **TDD red:** Change a RVA in the JSON → test still uses old hardcoded value → test fails. **TDD green:** Test uses generated value → test passes.
6. **Regression:** All tests pass.

**After TVS-3:** `test_orlib_resolver.cpp` no longer duplicates RVAs. Single source of truth flows from JSON → generated header → test assertions.

#### TVS-4: Evolve test_no_hardcoded_rvas.cpp

**Goal:** The forbidden-constants list is derived from the generated header, not hardcoded.

**Steps:**
1. Modify `test_no_hardcoded_rvas.cpp` to include `signatures.gen.h` and build the forbidden list from `kSignatures[]` (with patch-site entries using `rva + patchOffset`, the actual patch address) and `kDataGlobals[]` (Option A from §7.4). Exports are excluded.
2. **TDD red:** Add a new RVA to the JSON → test doesn't know about it → test passes erroneously. **TDD green:** Test reads from generated header → new RVA is automatically forbidden.
3. **Regression:** All tests pass.

**After TVS-4:** The RVA deduplication guard is itself deduplicated. No RVA appears in more than one place (the JSON file).

#### TVS-5: Archive experimental probes

**Goal:** Move 26 probe files from `src/` to `archive/`, remove from CMake build.

**Steps:**
1. Create `archive/` directory.
2. Move all probe files (`elevated_test*.cpp`, `hook_dll.cpp`, `inject.cpp`, `live_probe.cpp`, `tsdev_probe.cpp`, `sig_scan.cpp`) from `src/` to `archive/`.
3. Write `archive/README.md`.
4. Remove all probe targets from `CMakeLists.txt` (lines 31–57, 104–105).
5. **TDD red:** `cmake --build build --config Release` fails (missing source files for removed targets). **TDD green:** Build succeeds with only production + test targets.
6. **Regression:** `tsdev_cli`, `sigtool`, and all tests build and pass.

**After TVS-5:** `src/` contains only production code. Build is clean. 27 fewer targets in the build graph.

#### TVS-6: Move tests to tests/ directory

**Goal:** Separate test source files from production source files.

**Steps:**
1. Create `tests/` directory.
2. Move all `test_*.cpp` files from `src/` to `tests/`.
3. Update `CMakeLists.txt` source paths for all test targets.
4. Update `test_no_hardcoded_rvas.cpp` path resolution (it derives `src/tsdev_lib.cpp` relative to the executable — the executable moves but the source path derivation still works since it navigates up from `build\Release\`).
5. **TDD red:** Build fails (test sources not found at old paths). **TDD green:** Build succeeds with new paths.
6. **Regression:** All tests pass.

**After TVS-6:** Clean separation: `src/` = production, `tests/` = tests, `archive/` = probes.

#### TVS-7: Clean stale RVA comments + update BUILD.md

**Goal:** Remove RVA references from comments in `orlib_resolver.h` and `tsdev_lib.h`; update `BUILD.md` for the new structure.

**Steps:**
1. Update `orlib_resolver.h` Doxygen comments: remove hardcoded RVAs, replace with `\see signatures/v9.50.01.3344.json` references.
2. Update `tsdev_lib.h` comments: remove any RVA references.
3. Update `docs/BUILD.md`: document the new directory structure, the codegen step, the `sigtool` tool, and the fact that a fresh clone builds without `tmp/`.
4. **TDD red:** `grep -r "0x165de0\|0x165ed0\|..." src/ orlib_resolver.h tsdev_lib.h` finds matches in comments. **TDD green:** No matches outside the generated header and JSON.
5. **Regression:** All tests pass.

**After TVS-7:** No RVA appears in any source file comment. All RVAs live exclusively in the JSON source of truth and the generated header. `BUILD.md` is current.

### 9.3 TVS Dependency Graph

```
TVS-1 (JSON + codegen) ──────► TVS-2 (sigtool)
                        │
                        ├─────► TVS-3 (dedup test RVAs)
                        │              │
                        │              ▼
                        │         TVS-4 (evolve no-hardcoded-RVA test)
                        │
                        ├─────► TVS-5 (archive probes)
                        │
                        ├─────► TVS-6 (move tests to tests/)
                        │
                        └─────► TVS-7 (clean comments + BUILD.md)
```

TVS-1 is the prerequisite for all others. TVS-2 through TVS-7 can proceed in parallel (different subagents) after TVS-1 is complete and reviewed, though TVS-4 depends on TVS-3 (the test needs to include the generated header first).

---

## 10. Open Questions / Risks

### 10.1 Architectural Decisions Needing Validation

| # | Question | Status | Mitigation |
|---|---|---|---|
| Q1 | **Will `sigcodegen.py` generate byte-identical data to `b1_signatures.h`?** | Must be validated in TVS-1. The hex patterns, RVAs, and patch metadata must match exactly. | TVS-1 adversarial review includes a byte-level comparison of generated `kSignatures[]` vs. the original `b1_signatures.h` data. Any discrepancy is a blocker. |
| Q2 | **Should `sigtool` link `orlib_resolver` or extract a shared scanner library?** | Open. Linking `orlib_resolver` is simpler but pulls in version detection + patch logic. | Start with linking `orlib_resolver` (simplest). If the coupling causes issues, extract a `sig_scanner` static library in a later refactor. |
| Q3 | **Should `sigtool` use nlohmann/json for `--signatures` flag, or a minimal hand-rolled parser?** | Open. nlohmann/json is ~500KB but well-tested. Hand-rolled is zero-dependency but fragile. | Use nlohmann/json (single-header) included only in `sigtool.cpp`. The production CLI has no JSON dependency. If the dependency is objectional, a minimal parser for the known schema is ~100 lines. |
| Q4 | **Should the generated header be in `build/generated/` or in `src/`?** | Decided: `build/generated/`. It's a build artifact, not source. | This means `#include "signatures.gen.h"` resolves via `target_include_directories(... PRIVATE ${GEN_INCLUDE_DIR})`. Verified that MSVC supports out-of-source include directories. |
| Q5 | **Should `test_no_hardcoded_rvas.cpp` use Option A (read from generated header) or Option B (keep hardcoded list)?** | Decided: Option A (final state), Option B (transitional in TVS-1). | Option A is implemented in TVS-4 after the codegen pipeline is stable. |

### 10.2 Migration Risks

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| R1 | **Codegen correctness:** `sigcodegen.py` generates incorrect hex patterns or RVAs, causing signature scan failures. | Medium | High — production CLI breaks. | TVS-1 includes a validation step: run `sigtool verify` (or `test_orlib_resolver`) against the baseline DLL with the generated header. If all 12 signatures match, the codegen is correct. Additionally, adversarial review compares generated data byte-by-byte against `b1_signatures.h`. |
| R2 | **CMake codegen timing:** The codegen step doesn't run before `orlib_resolver` compiles on the first build (missing dependency). | Low | High — build failure with missing header. | Use `add_dependencies(orlib_resolver siggen)` (not just `target_link_libraries`). The `add_custom_command` `DEPENDS` list includes all JSON files + the script, ensuring regeneration on any change. Test with a clean build directory. |
| R3 | **Python availability:** The build machine doesn't have Python installed. | Low (machine has VS Build Tools 2026; Python is commonly available) | High — codegen can't run. | `find_program(PYTHON_EXECUTABLE python REQUIRED)` fails at configure time with a clear message. Document Python as a build prerequisite in `BUILD.md`. |
| R4 | **Ghidra script compatibility:** `GenerateSignatures.py` doesn't work with the installed Ghidra version (Jython vs PyGhidra). | Medium | Medium — signature regeneration requires manual fallback. | Use `#@runtime PyGhidra` directive. Test the script against the installed Ghidra version in a standalone TVS (not part of the core migration). The script is a developer tool, not a build dependency — its failure doesn't break the production build. |
| R5 | **Test path resolution:** `test_no_hardcoded_rvas.cpp` derives `src/tsdev_lib.cpp` relative to the executable. After moving tests to `tests/`, the path may break. | Medium | Low — test fails with "file not found" (exit 2), not a false pass. | The test navigates up from `build\Release\` to the repo root, then into `src\`. This works regardless of where the test source lives. Verify in TVS-6. |
| R6 | **Probe archival breaks a reference:** Someone was using `elevated_test20.cpp` as a reference and it's now in `archive/` (not compiled). | Low | Low — the file is still readable, just not compiled. | `archive/README.md` documents where each file went and why. The files are in git history and in `archive/`. |
| R7 | **JSON schema evolution:** Future signature sets need fields not in the current schema. | Low | Low — schema versioning handles this. | `schemaVersion` field in both JSON formats. `sigcodegen.py` validates against the expected schema and fails loudly on unknown fields or missing required fields. |
| R8 | **Multi-version selection untested:** The runtime signature set selection logic is designed but only one version exists. | High (inherent) | Low — with one version, the selection is trivial. | The design is conservative: unknown versions get a warning and proceed with the default set. The multi-version path is exercised only when a second version is added, at which point it's tested against a real DLL. |

### 10.3 Risk Mitigation Summary

The highest-risk item is **R1 (codegen correctness)**. The mitigation is thorough: TVS-1 includes byte-level comparison + functional regression tests + adversarial review. If the codegen produces incorrect data, the signature scan tests will fail immediately (no-match or multi-match), providing a clear signal.

The second highest risk is **R2 (CMake timing)**. The mitigation is standard CMake best practices: `add_custom_command` with proper `DEPENDS`, `add_custom_target`, and `add_dependencies`. A clean-build test in TVS-1 validates this.

All other risks are low-impact or have straightforward mitigations. The migration is designed so that each TVS is independently testable and the production CLI works end-to-end after every step.

---

## Appendix A: File Move Map

| Current location | Target location | TVS |
|---|---|---|
| `tmp/re_artifacts/b1_signatures.h` | Stays in `tmp/` (historical reference, no longer a build dep) | TVS-1 |
| (new) | `signatures/v9.50.01.3344.json` | TVS-1 |
| (new) | `signatures/version_manifest.json` | TVS-1 |
| (new) | `tools/sigcodegen.py` | TVS-1 |
| (new) | `src/sigtool.cpp` | TVS-2 |
| `src/test_orlib_resolver.cpp` | `tests/test_orlib_resolver.cpp` | TVS-6 |
| `src/test_insert.cpp` | `tests/test_insert.cpp` | TVS-6 |
| `src/test_delete.cpp` | `tests/test_delete.cpp` | TVS-6 |
| `src/test_delete_check.cpp` | `tests/test_delete_check.cpp` | TVS-6 |
| `src/test_delete_diag.cpp` | `tests/test_delete_diag.cpp` | TVS-6 |
| `src/test_modify.cpp` | `tests/test_modify.cpp` | TVS-6 |
| `src/test_persistence_validation.cpp` | `tests/test_persistence_validation.cpp` | TVS-6 |
| `src/test_no_hardcoded_rvas.cpp` | `tests/test_no_hardcoded_rvas.cpp` | TVS-6 |
| `src/elevated_test*.cpp` (21 files) | `archive/elevated_test*.cpp` | TVS-5 |
| `src/hook_dll.cpp` | `archive/hook_dll.cpp` | TVS-5 |
| `src/inject.cpp` | `archive/inject.cpp` | TVS-5 |
| `src/live_probe.cpp` | `archive/live_probe.cpp` | TVS-5 |
| `src/tsdev_probe.cpp` | `archive/tsdev_probe.cpp` | TVS-5 |
| `src/sig_scan.cpp` | `archive/sig_scan.cpp` | TVS-5 |

## Appendix B: CMake Target Map (Before vs After)

| Target | Before (current) | After (target) |
|---|---|---|
| `tsdev_probe` | `add_executable(src/tsdev_probe.cpp)` | **Removed** (archived) |
| `live_probe` | `add_executable(src/live_probe.cpp)` | **Removed** (archived) |
| `elevated_test` through `elevated_test22` | 22 `add_executable` lines (21 source files + 1 duplicate target compiling `elevated_test21.cpp`) | **Removed** (archived) |
| `hook_dll` | `add_library(SHARED src/hook_dll.cpp)` | **Removed** (archived) |
| `inject` | `add_executable(src/inject.cpp)` | **Removed** (archived) |
| `sig_scan` | `add_executable(src/sig_scan.cpp)` | **Removed** (archived, superseded by sigtool) |
| `tsdev_lib` | `add_library(STATIC src/tsdev_lib.cpp)` | **Unchanged** |
| `orlib_resolver` | `add_library(STATIC src/orlib_resolver.cpp)` | **Modified**: includes `generated/` dir, depends on `siggen` |
| `tsdev_cli` | `add_executable(src/tsdev_cli.cpp)` | **Unchanged** |
| `test_insert` | `add_executable(src/test_insert.cpp)` | **Modified**: source path → `tests/` |
| `test_delete` | `add_executable(src/test_delete.cpp)` | **Modified**: source path → `tests/` |
| `test_delete_check` | `add_executable(src/test_delete_check.cpp)` | **Modified**: source path → `tests/` |
| `test_delete_diag` | `add_executable(src/test_delete_diag.cpp)` | **Modified**: source path → `tests/` |
| `test_modify` | `add_executable(src/test_modify.cpp)` | **Modified**: source path → `tests/` |
| `test_persistence_validation` | `add_executable(src/test_persistence_validation.cpp)` | **Modified**: source path → `tests/` |
| `test_no_hardcoded_rvas` | `add_executable(src/test_no_hardcoded_rvas.cpp)` | **Modified**: source path → `tests/` |
| `test_orlib_resolver` | `add_executable(src/test_orlib_resolver.cpp)` | **Modified**: source path → `tests/`, includes `generated/` dir |
| (new) `siggen` | — | `add_custom_target` (codegen) |
| (new) `sigtool` | — | `add_executable(src/sigtool.cpp)` |
