# Signature Catalog — orlib20.dll Code-Section RVA Targets

> **CORRECTION NOTICE — 2026-08-26 (Workstream M, slice M.3).**
>
> Workstream M slice M.3 (TSDev parity — 0x40A post-success library-change
> notification) has **added** the ordinal-only `[NONAME]` export **57**
> (`FUN_101526f0`, the asynchronous 0x40A sender) to the tracked export
> list. Production code now resolves it via `GetProcAddress` with
> `MAKEINTRESOURCEA(57)` (production path) or PE export-table indexing
> (test-mode path), and `verifyModule()` (success path) plus `deleteEntry()`
> (success path, gated `category != 4`) call it to post the 0x40A
> notification, matching TSDev's historical Frida-confirmed ordering. See new §5
> entry "Ordinal 57". The cited raw trace under `tmp/` is gitignored,
> transient, and unavailable as tracked current-run evidence.
>
> **Current totals (canonical — authoritative):** Signature definitions:
> **22** (ordinal 57 is an export, not a code-section signature). Tracked
> exports: **13** (10 by-name plus ordinal-only 66, 67, and 57; this is the
> resolver's tracked-export count, not the total PE export count — the
> baseline PE export table has 699 total entries). Data globals: **3**. Patch
> sites: **11**. Generated header constants: `kSignatureCount=22`,
> `kExportCount=13`. `tools/sigcodegen.py` constants:
> `EXPECTED_SIGNATURE_COUNT=22`, `EXPECTED_DATA_GLOBAL_COUNT=3`,
> `EXPECTED_EXPORT_COUNT=13`. All other internal mentions of these counts in
> this file refer back to this statement.
>
> The M.3 transition was **12 → 13 tracked exports** (ordinal 57 added); prior
> unqualified current-looking "12 exports" references are superseded.
>
> Per the project's documentation rules, historical banners and sections
> below are **not silently rewritten**; dated corrections are prepended
> instead. The 2026-08-24 M.1 correction notice (which describes the
> 24 → 22 signature / 10 → 12 export transition) is retained as historical
> context and is **superseded by this notice** with respect to the export
> count only. Signature *facts* (RVAs, original/replacement bytes, scan
> results) for the 22 retained signatures remain authoritative and
> unchanged.
>
> **Historical snapshots (consolidated):** Each prior count phase is recorded
> once here with its provenance; other internal mentions of historical counts
> reference this subsection. Counts are given as **signatures / tracked
> exports / data globals / patch sites**.
>
> | Phase | Sigs | Exports | Globals | Patches | Provenance |
> |---|---:|---:|---:|---:|---|
> | Pre-L.3 | 25 | 10 | 3 | 12 | Before TVS-L.3 retired target #9 (`FUN_10011230_persistence_patch`). See the L.3 records below and `docs/workstream_unknown_link_fix_plan.md`. |
> | Post-L.3, pre-M.1 (F.4/F.5 snapshot) | 24 | 10 | 3 | 11 | After TVS-L.3 removed target #9; F.4/F.5 added 13 signatures and 3 exports. See the F.4/F.5 snapshot below. |
> | Post-M.1, pre-M.3 | 22 | 12 | 3 | 11 | M.1 retired `CELCompiler_ctor`/`CELCompiler_dtor` signatures (§4.3, §4.4) and added ordinal-only exports 66/67. See the M.1 notice below and `docs/M1_HANDOFF.md`. |
> | **Current (post-M.3)** | **22** | **13** | **3** | **11** | M.3 added ordinal-only export 57. See Current totals above. |
>
> **Evidence:** `docs/RE/RE_M3_0x40A_NOTIFICATION.md` §1.2 (R-M.3.1) —
> Ghidra export-table annotation `/* 0x1526f0  57   */` confirming
> ordinal 57 = `FUN_101526f0` (`MOVZX ECX,word[EBP+8]` category,
> `MOVZX EAX,word[EBP+0xC]` ProgId, `SHL ECX,0x10`, `OR ECX,EAX`,
> `PUSH ECX` lParam, `PUSH 0` wParam, `PUSH 0x40A` msg,
> `CALL [tslib!PosMessageToClients]`, `MOV EAX,1`, `RET 8`); historical/transient
> Frida trace `tmp/frida_trace_verify.log` (unavailable as tracked current-run
> evidence) recorded 57 `PostMessageW(0x40A)` sends to 19 registered HWNDs
> after verify success.

> **HISTORICAL CORRECTION NOTICE — 2026-08-24 (Workstream M, slice M.1).**
>
> Workstream M slice M.1 (TSDev parity — compiler-object lifecycle) has
> **retired** the `CELCompiler_ctor` (§4.3) and `CELCompiler_dtor` (§4.4)
> code-section RVA signatures. Production code no longer resolves the
> CELCompiler constructor/destructor by RVA signature; instead it resolves
> them as **ordinal-only `[NONAME]` exports 66 and 67** of `orlib20.dll`
> via `GetProcAddress` with `MAKEINTRESOURCEA(ordinal)` (production path) or
> PE export-table indexing (test-mode path). See new §5 entries "Ordinal 66"
> and "Ordinal 67" and the RETIRED notes prepended to §4.3 and §4.4.
>
> **Catalog totals change as a result:** M.1 transitioned the catalog from
> the post-L.3/pre-M.1 phase (24/10/3/11) to the post-M.1/pre-M.3 phase
> (22/12/3/11) — see the Historical snapshots subsection above for the full
> count lineage and provenance. The current post-M.3 totals (22/13/3/11) are
> in the Current totals statement above. Generated header constants and
> `tools/sigcodegen.py` `EXPECTED_*` constants track the current phase.
>
> Per the project's documentation rules, historical banners and sections
> below are **not silently rewritten**; dated corrections are prepended
> instead. The historical TVS-L.3 cleanup note and 2026-08-23 supersession
> banner are retained as historical context and are **superseded by this
> notice** with respect to signature/export counts only. Signature *facts*
> (RVAs, original/replacement bytes, scan results) for the 22 retained
> signatures remain authoritative and unchanged.
>
> **Historical/transient evidence (unavailable in the tracked checkout):** `tmp/m1_ghidra_findings.md` §1 (R-M.1.1) — Ghidra export
> table annotations + xrefs confirming ordinal 66 = FUN_10165f70
> (`operator_new(0x58)` + ctor FUN_10165a00, cdecl, no args, returns
> `void*`) and ordinal 67 = FUN_10165fd0 (dtor FUN_10165ac0 + `operator
> delete(p, 0x58)`, stdcall, one arg `void* p`, returns void). Full M.1
> findings/handoff: `docs/M1_HANDOFF.md`.
>
> **Historical verification status (post-M.1, pre-M.3; unverified historical/transient; not current acceptance evidence):** 22/22 signatures PASS;
> `test_orlib_resolver` 251/251 × 3 cycles, 0 failures; `test_sigtool`
> 12/12 passed; `test_compiler_layout` 18/18 assertions, 0 failures;
> `test_no_hardcoded_rvas` 25 forbidden entries, 0 violations; full
> regression 12/12 suites PASS. See `docs/M1_HANDOFF.md` for the complete
> test and E2E results.

> **Historical status note (pre-closure):** Earlier Workstream L status text was
> superseded by the current closure status and is retained only for provenance;
> it is not current workstream guidance.
>
> **CURRENT-STATUS BANNER — Workstream L.** The current Workstream L/M status
> (closure of L, M.1/M.2/M.3 approved historical, M.3b planned, M.4
> manual/blocked, M.5 pending) is maintained in
> `AGENTS.md §"Current state and TODO"`; that file is the SSOT for
> workstream-status sentences and should be consulted instead of the
> verbatim status text previously carried here.
>
> The historical L.1–L.17 claims, results, and fault records below are retained
> for provenance. Signature facts (counts, RVAs,
> original/replacement bytes, scan results) below are separate from those
> workstream-status records.
>
> **HISTORICAL STATUS RECORD — Workstream L (pre-closure):** At the time of
> this record, TVS-L.1–L.3 were recorded as complete, while the L.4/L.5
> completion claim had been falsified by persistence and deletion fault
> injection and was recorded as superseded by the L.6–L.15 recovery. The
> historical fault details, branch references, and L.17 gate caveats are
> retained in the dated L records; none is current guidance after the closure
> decision above.

> **HISTORICAL TVS-L.3 CLEANUP NOTE (post-L.3, pre-M.1).** The
> `FUN_10011230_persistence_patch` entry (former target #9, §3 row 9, §4.9)
> has been **removed** from this catalog as part of TVS-L.3
> (`docs/workstream_unknown_link_fix_plan.md`). Catalog totals after L.3 were
> 24/10/3/11 (see the Historical snapshots subsection above). Target #9 is
> **retired as a stable-ID gap** — targets 1–8 and 10–25 retain their original
> IDs; generated array indices must not be inferred from IDs. The 2026-08-23
> supersession banner below is **retained as historical context**; it
> documents the root-cause finding (caller `ADD ESP,0x0C` at `0x1001E0C1`,
> original plain `RET` at `0x10011454`, patched `RET 0x0C` as callee-cleanup
> ABI hazard) that motivated the removal. The durable evidence remains in
> `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`.

> **HISTORICAL SUPERSESSION BANNER — 2026-08-23 (Workstream L root-cause confirmation).**
>
> Confirmed Unknown-link findings have invalidated the **calling-convention
> narrative** for the `FUN_10011230_persistence_patch` entry (target #9,
> §3 row 9, §4.9). The signature/patch *facts* below (counts, RVAs, original
> bytes, replacement bytes, scan results) are **retained as-is** because the
> pre-L.3 production code still carried the patch until TVS-L.3 removed it;
> the catalog must not prematurely drop counts or entries. The following
> narrative claims are **stale/superseded**:
>
> - The `__fastcall` (ECX=arg1, EDX=arg2) "with 12 bytes stack args
>   (RET 0x0C)" / "the patch's `RET 0x0C` cleans 12 bytes" framing in
>   §3 row 9, §4.9 calling-convention field, and §4.9 structural assumptions
>   is **stale**. Durable Ghidra evidence
>   (`docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`) shows the original callee ends in
>   **plain `RET`** at `0x10011454` and the caller executes **`ADD ESP,0x0C`**
>   at `0x1001E0C1`: the original ABI is fastcall-like register passing with
>   **caller cleanup** for the 12 stack bytes. The patch's `RET 0x0C` changes
>   that cleanup contract to callee cleanup — a static ABI hazard, not a
>   correct ABI match. No claim is made that an observed historical crash was
>   caused by it.
> - The §4.9 "Patch details" framing that the patch "makes `SymWriter::Save`
>   return success so the `.ell` file is written" and "bypasses a .NET
>   `XmlDocument.Save` crash in `ELDebugger.dll`" is **stale**. The patch is
>   the **bypass that produces runtime-unlinkable output**: it skips
>   `FUN_1008E640` (compiler/link-finalization), `SymInfo::AdjustOffsets`, and
>   `SymWriter::Save`, then forces `EAX=1` so `CompileText` proceeds to
>   `ModuleInfo::SetGuid` / `MDVault::SaveModule` and marks the item verified
>   despite an incomplete finalization. The real prerequisite is **per-thread
>   STA COM** for the MSXML wrapper used by `SymWriter::Save` (not "partially
>   initialized CLR").
>
> **HISTORICAL PATCH-REMOVAL PLAN:** The `FUN_10011230_persistence_patch` entry
> (target #9) is slated for **complete removal** in TVS-L.3
> (`docs/workstream_unknown_link_fix_plan.md`), after TVS-L.2 establishes the
> STA COM prerequisite that makes the unpatched path safe to run. When L.3
> shipped, the catalog totals transitioned from the pre-L.3 phase (25/10/3/12)
> to the post-L.3 phase (24/10/3/11) — see the Historical snapshots subsection
> above. **Before L.3 shipped, the then-current pre-L.3 counts and target #9
> entry were required to remain in this catalog and in the pre-L.3 signature
> JSON snapshot** — the pre-L.3 production code still applied this patch.
>
> **HISTORICAL L.3 UPDATE — shipped:** TVS-L.3 had shipped at the time of this record. The removal described
> above is **complete**: the target #9 entry and §4.9 have been removed, and
> count references throughout this catalog and in
> `signatures/v9.50.01.3344.json` were updated to the post-L.3 phase (see the
> Historical snapshots subsection above). The preceding paragraphs are retained
> as the historical 2026-08-23 record of the root-cause finding. See the
> TVS-L.3 cleanup note at the very top of this file.
>
> **HISTORICAL PRE-L.3 STATUS (retained):** Production runtime acceptance was
> recorded as **BROKEN**: CLI-produced strategies failed in TradeStation
> Desktop with "Unknown link error occurred" despite TSDev reporting them
> VERIFIED. Workstream L was then recorded as **PLANNED but NOT implemented**. Durable evidence: `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`;
> approved plan: `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`; L plan:
> `docs/workstream_unknown_link_fix_plan.md`; dynamic A/B proof:
> `tmp/com_unpatched_probe/`, `tmp/com_nopatch_6689.ell`,
> `tmp/com_nopatch_6689.xml`.
>
> **[L.3 UPDATE — shipped:** TVS-L.3 has now shipped; the persistence bypass
> is removed and the real finalization path runs. See the TVS-L.3 cleanup
> note at the top of this file and `docs/workstream_unknown_link_fix_plan.md`
> for the current Workstream L status.**]**
>
> **[RECONCILIATION — current status, 2026-08-26:** The current Workstream L/M
> status is maintained in `AGENTS.md §"Current state and TODO"` (the SSOT for
> workstream-status sentences); the verbatim L/M status text previously
> carried here has been removed in favor of that reference. The earlier L
> status text is retained as historical context. Signature facts below are
> separate from these workstream-status records.**]**

| Field | Value |
|---|---|
| **TVS Identifier** | TVS-B.1 + Workstreams F.4, F.5 |
| **Date** | 2026-08-17 (B.1); 2026-08-19/26 (F.4, F.5 additions) |
| **Status** | APPROVED |
| **Purpose** | Catalogs the 22 active code-section RVA targets in `orlib20.dll`. The current provenance is `(12 original TVS-B.1 definitions − 1 L.3-retired persistence target − 2 M.1-retired compiler-object targets) + 13 F.4/F.5 definitions = 22`; the retired targets remain only as explicitly labeled historical rows. It provides for each active target: a candidate byte signature (hex with `??` wildcard notation), signature length, uniqueness-verification result (scan count = 1 within `.text`), expected original bytes at the patch site (for patches), calling convention, and structural assumptions. It also documents the DLL baseline metadata (version, PE timestamp, SHA-256), the 13 tracked export entries (10 by-name exports plus 3 ordinal-only `[NONAME]` exports 66, 67, and 57), 3 data-section RVA targets, 11 patch sites, and structural constants. The 13 tracked exports are not the total PE export count. This catalog is the human-readable reference for signature-based RVA resolution and version detection. The authoritative machine-readable source of truth is `signatures/v9.50.01.3344.json`; `tools/sigcodegen.py` enforces 22 definitions, 3 data globals, and 13 tracked exports. |

> **WORKSTREAM E NOTICE (2026-08-17):** The signature data source of truth has been migrated from `tmp/re_artifacts/b1_signatures.h` (gitignored, not regenerable) to version-controlled `signatures/v9.50.01.3344.json` with build-time C++ code generation. See `docs/archive/ARCHITECTURE_REDESIGN.md` for the complete design and TVS-E.1 through TVS-E.7 migration plan. The signature patterns and metadata in this document remain valid; only the storage format and build integration changed. This migration is **IMPLEMENTED (Workstream E complete)**.

> **HISTORICAL WORKSTREAM F.4/F.5 SNAPSHOT (2026-08-19/26, pre-M.1):** 13 new signatures and 3 new exports were added to `signatures/v9.50.01.3344.json` during Workstreams F.4 (get-source) and F.5 (case-insensitive exists). The post-F.4/F.5 phase counts were 24/10/3/11 (see the Historical snapshots subsection above). The JSON file (`signatures/v9.50.01.3344.json`) is the **authoritative source of truth**; this catalog is the human-readable reference. All counts are validated at build time by `tools/sigcodegen.py`.
>
> **[HISTORICAL M.1 SNAPSHOT — 2026-08-24 (pre-M.3):** The counts in this notice are superseded by Workstream M.1. M.1 retired the `CELCompiler_ctor` and `CELCompiler_dtor` signatures (§4.3, §4.4) and added ordinal-only exports 66 and 67 (see §5). Post-M.1 phase counts were 22/12/3/11 (see the Historical snapshots subsection above). The F.4/F.5 narrative above is retained as the historical record of the F.4/F.5 additions; only the totals are superseded. See the 2026-08-24 correction notice at the top of this file and `docs/M1_HANDOFF.md`.**]

> **Transient evidence note:** The `tmp/re_artifacts/` paths listed below are
> gitignored historical artifacts, not tracked repository evidence. Missing
> entries are unavailable in the current checkout; no replacement scan,
> inventory, or review result is implied. Current catalog facts come from the
> tracked JSON, `tools/sigcodegen.py`, and current source.

**Source artifacts (historical/transient; not checked in):**

| Artifact | Content |
|---|---|
| `b1_dll_baseline.md` | DLL baseline metadata (version, SHA-256, PE timestamp, .text section info) |
| `b1_rva_inventory.md` | Full inventory of 46 entries (12 original RVA targets + 3 data offsets + 7 original export names + structural constants) |
| `b1_ghidra_evidence.md` | Ghidra disassembly, raw bytes, calling conventions for the original 12 targets |
| `b1_signatures.h` | The original 12 finalized signatures (hex patterns with `??` wildcards, patch metadata) — **superseded by `signatures/v9.50.01.3344.json` (Workstream E); kept as historical reference** |
| `b1_export_confirmation.md` | Export table confirmation for the original 7 export names |
| `b1_sig_scan_run.md` | Uniqueness scan results (12/12 PASS, exit 0) for the original 12 signatures |
| `signatures/v9.50.01.3344.json` | **Authoritative source of truth** — 22 signature definitions, 3 data globals, 13 tracked exports (all versions; post-M.3; tracked exports are not the total PE export count) |

---

## 1. DLL Baseline

*Historical source (transient/unavailable in the tracked checkout): `tmp/re_artifacts/b1_dll_baseline.md`; current tracked baseline reference: §1 and `tests/fixtures/orlib20_baseline.dll`.*

All measurements taken from the copied baseline DLL, not the read-only install original.

| Field | Value |
|---|---|
| **Source (read-only install)** | `C:\Program Files (x86)\TradeStation 9.5\Program\orlib20.dll` |
| **Copied artifact** | `tests/fixtures/orlib20_baseline.dll` (committed to repo) |
| **FileVersion** | 9.50.01.3344 |
| **ProductVersion** | 9.50.01.3344 |
| **FileDescription** | Omega EasyLanguage |
| **CompanyName** | TradeStation Technologies, Inc. |
| **File size** | 4,924,400 bytes |
| **SHA-256** | `318DB24A4F41BDB9FF69E2E647026B788706904CF887CBB0A953E003DB22EC18` |
| **PE timestamp (TimeDateStamp)** | `0x5C01735E` = Fri Nov 30 17:29:02 2018 UTC |
| **Machine** | 0x014C (x86) |
| **Characteristics** | 0x2102 (Executable, 32-bit word machine, DLL) |
| **Magic** | 0x010B (PE32) |
| **Linker version** | 14.00 |
| **Entry point (RVA)** | 0x1EB235 (VA 0x101EB235) |
| **Image base** | `0x10000000` (range 0x10000000–0x104BAFFF) |
| **.text RVA (virtual address)** | `0x00001000` |
| **.text virtual size** | `0x001F72FC` (2,061,052 bytes) |
| **.text raw size (size of raw data)** | `0x001F7400` (2,061,312 bytes) |
| **.text file pointer to raw data** | `0x00000400` |

**Summary:** Version 9.50.01.3344 (Omega EasyLanguage, TradeStation Technologies, Inc.), 4,924,400 bytes, SHA-256 `318DB24A...`, PE timestamp `0x5C01735E` (Fri Nov 30 17:29:02 2018 UTC), image base `0x10000000`, .text RVA `0x00001000`, virtual size `0x001F72FC`, raw size `0x001F7400`.

---

## 2. Wildcard Notation Convention

*Current source: `signatures/v9.50.01.3344.json` (header comment); historical Ghidra source (transient/unavailable in the tracked checkout): `tmp/re_artifacts/b1_ghidra_evidence.md` (§Wildcard Notation Convention).*

In all signatures in this catalog, `??` represents a **wildcard byte** (matches any single byte at that position). All other bytes are **fixed** and must match exactly.

### What is wildcarded (`??`)

| Category | Bytes | Rationale |
|---|---|---|
| **SEH handler addresses** | 4 bytes at prologue offset 6–9 (or 13–16 for GetWarehouse) | Absolute, image-base-relative addresses that change if the DLL is relocated or rebuilt |
| **Data-section references** | 4 bytes in `MOV reg, [imm32]` / `MOV [imm32], reg` | Absolute, image-base-relative pointers into `.data` (e.g., `DAT_1023cea0`) |
| **IAT indirect call targets** | 4 bytes in `CALL [imm32]` (`FF 15 imm32`) | IAT slot addresses are image-base-relative |
| **Relative call/jump targets** | 4 bytes in `CALL rel32` (`E8 imm32`) and `JZ rel32` (`0F 8x imm32`) | Relative displacements change if code shifts |

### What is fixed (NOT wildcarded)

| Category | Examples |
|---|---|
| **Opcode bytes** | `55` (PUSH EBP), `8B EC` (MOV EBP,ESP), `E8` (CALL rel32), `FF 15` (CALL [imm32]), etc. |
| **ModR/M bytes** | Register encodings, addressing mode bytes |
| **Register encodings** | e.g., `8B F9` (MOV EDI,ECX), `8B F2` (MOV ESI,EDX) |
| **Structural immediates** | Stack frame sizes (`81 EC 68 01 00 00` = SUB ESP,0x168), error record stride (`69` = IMUL prefix with 0x129), error record size (`68 B1 00 00 00` = PUSH 0xB1), allocation sizes (`6A 2C` = PUSH 0x2C) |
| **Short jump displacements** | `74 15` (JZ +0x15), `74 01` (JZ +1), `75 19` (JNZ +0x19), `74 62` (JZ +0x62) — 1-byte displacements kept fixed |
| **FS:[0] encoding zeros** | The `00 00 00 00` in `64 A1 00 00 00 00` (MOV EAX,FS:[0]) and `64 89 25 00 00 00 00` (MOV FS:[0],ESP) — always zero in this encoding |

---

## 3. Signature Catalog Table

*Current source: `signatures/v9.50.01.3344.json` (exact hex patterns); historical scan source (transient/unavailable in the tracked checkout): `tmp/re_artifacts/b1_sig_scan_run.md`.*

One row per RVA target (22 active rows; target #9 retired in TVS-L.3 and targets #3 `CELCompiler_ctor` / #4 `CELCompiler_dtor` retired in Workstream M.1 — stable-ID gaps, not renumbered). The two M.1-retired rows are retained below marked **RETIRED 2026-08-24 (M.1)** for historical traceability; they are no longer part of the active 22-signature set and are not generated into `signatures/v9.50.01.3344.json`. For the 11 patch-site rows (targets 10–16, 19–21, and 25), additional patch metadata columns are populated.

> **Note on patch-site targets:** The signature matches the **containing function** entry point (or a mid-function anchor for some F.5 signatures), not the patch byte itself. The "RVA" column shows the **patch-site RVA** (the production target). The "Sig Match RVA" column shows where the signature actually matches (the containing function entry or anchor). The patch byte is at `Sig Match RVA + Patch Offset`.

| # | Name | RVA (target) | Calling Convention | Length | Unique? | Patch Site? |
|---|---|---|---|---|---|---|
| 1 | StartCompilingProcess | 0x165DE0 | `__cdecl`, no args | 64 | Yes (1/1) | No |
| 2 | EndCompilingProcess | 0x165ED0 | `__fastcall`, 1 arg (int param) | 55 | Yes (1/1) | No |
| 3 | CELCompiler_ctor — **RETIRED 2026-08-24 (M.1)** | 0x165A00 | `__fastcall` (ecx=this) | 64 | Yes (1/1) | No |
| 4 | CELCompiler_dtor — **RETIRED 2026-08-24 (M.1)** | 0x165AC0 | `__fastcall` (ecx=this) | 38 | Yes (1/1) | No |
| 5 | InitGlobalLookup | 0x1A9460 | `__cdecl`, no args | 66 | Yes (1/1) | No |
| 6 | DeleteFromTree | 0x1E4030 | `__thiscall` (ecx=globalLookup; stack: whType, progId, flag); RET 12 | 66 | Yes (1/1) | No |
| 7 | GetWarehouse | 0x1A9650 | `__cdecl`, 1 arg (path; 0=default) | 45 | Yes (1/1) | No |
| 8 | FUN_101a8360_delete_orchestrator | 0x1A8360 | `__fastcall` (ECX=whType, EDX=progId, no stack args) | 64 | Yes (1/1) | No |
| 10 | FUN_101d0950_thread_check_INT3 | 0x1D0993 | `__thiscall` (ecx=this) | 36 | Yes (1/1) | Yes |
| 11 | FUN_101cf5b0_thread_check_INT3 | 0x1CF5D4 | `__thiscall` (ecx=this) | 37 | Yes (1/1) | Yes |
| 12 | FUN_101d1fa0_thread_check_INT3 | 0x1D1FC8 | `__thiscall` (ecx=this) | 57 | Yes (1/1) | Yes |
| 13 | FUN_101d0e80_thread_check_INT3 | 0x1D0EB9 | `__thiscall` (ecx=this) | 74 | Yes (1/1) | Yes |
| 14 | FUN_101d0c30_thread_check_INT3 | 0x1D0C58 | `__thiscall` (ecx=this) | 51 | Yes (1/1) | Yes |
| 15 | FUN_101d16c0_thread_check_INT3 | 0x1D1714 | `__thiscall` (ecx=this) | 132 | Yes (1/1) | Yes |
| 16 | FUN_101d0d90_thread_check_INT3 | 0x1D0DBF | `__thiscall` (ecx=this) | 58 | Yes (1/1) | Yes |
| 17 | FUN_101d43a0_build_type_array | 0x1D43A0 | `__stdcall` (2 stack args; RET 8) | 48 | Yes (1/1) | No |
| 18 | FUN_1000dd30_celprogwriter_ctor | 0xDD30 | `__fastcall` (ecx=this) | 63 | Yes (1/1) | No |
| 19 | FUN_101cfce0_thread_check_INT3 | 0x1CFD08 | `__thiscall` (ecx=this) | 51 | Yes (1/1) | Yes |
| 20 | FUN_101d15c0_thread_check_INT3 | 0x1D1614 | `__thiscall` (ecx=this) | 95 | Yes (1/1) | Yes |
| 21 | FUN_101d18c0_thread_check_INT3 | 0x1D19E6 | `__thiscall` (ecx=this) | 26 | Yes (1/1) | Yes |
| 22 | FUN_101a42d0_find_item_get_name | 0x1A42D0 | `__fastcall` (ECX=warehouse, EDX=name) | 81 | Yes (1/1) | No |
| 23 | FUN_101d05e0_bst_search_wrapper | 0x1D05E0 | `__thiscall` (ecx=warehouse; stack: typeFilter, name, *outNode); RET 0xC | 74 | Yes (1/1) | No |
| 24 | FUN_101c36d0_verbatim_name_getter | 0x1C36D0 | `__fastcall` (ecx=node pointer) | 80 | Yes (1/1) | No |
| 25 | FUN_101c36d0_thread_check_INT3 | 0x1C373A | `__fastcall` (ecx=node pointer) | 14 | Yes (1/1) | Yes |

### Patch-site metadata

| # | Name | Patch-Site RVA | Sig Match RVA (containing fn) | Patch Offset | Original Bytes | Replacement Bytes |
|---|---|---|---|---|---|---|
| 10 | FUN_101d0950_thread_check_INT3 | 0x1D0993 | 0x1D0950 | 0x43 (67) | `CC` | `90` |
| 11 | FUN_101cf5b0_thread_check_INT3 | 0x1CF5D4 | 0x1CF5B0 | 0x24 (36) | `CC` | `90` |
| 12 | FUN_101d1fa0_thread_check_INT3 | 0x1D1FC8 | 0x1D1FA0 | 0x28 (40) | `CC` | `90` |
| 13 | FUN_101d0e80_thread_check_INT3 | 0x1D0EB9 | 0x1D0E80 | 0x39 (57) | `CC` | `90` |
| 14 | FUN_101d0c30_thread_check_INT3 | 0x1D0C58 | 0x1D0C30 | 0x28 (40) | `CC` | `90` |
| 15 | FUN_101d16c0_thread_check_INT3 | 0x1D1714 | 0x1D16C0 | 0x54 (84) | `CC` | `90` |
| 16 | FUN_101d0d90_thread_check_INT3 | 0x1D0DBF | 0x1D0D90 | 0x2F (47) | `CC` | `90` |
| 19 | FUN_101cfce0_thread_check_INT3 | 0x1CFD08 | 0x1CFCE0 | 0x28 (40) | `CC` | `90` |
| 20 | FUN_101d15c0_thread_check_INT3 | 0x1D1614 | 0x1D15C0 | 0x54 (84) | `CC` | `90` |
| 21 | FUN_101d18c0_thread_check_INT3 | 0x1D19E6 | 0x1D19E4 | 0x02 (2) | `CC` | `90` |
| 25 | FUN_101c36d0_thread_check_INT3 | 0x1C373A | 0x1C3736 | 0x04 (4) | `CC` | `90` |

---

## 4. Per-Target Detail Sections

*Current source: `signatures/v9.50.01.3344.json` (exact signatures); historical Ghidra and scan sources (transient/unavailable in the tracked checkout): `tmp/re_artifacts/b1_ghidra_evidence.md` and `tmp/re_artifacts/b1_sig_scan_run.md`.*

**Export status note (applies to all 22 active targets):** None of the 22 active RVA targets are exported functions. They are **internal functions** resolved by RVA (`m_orlibBase + RVA`), not by `GetProcAddress`. The 13 tracked exported entries used in production code (10 by-name exports plus 3 ordinal-only `[NONAME]` exports 66, 67, and 57) are documented separately in §5. The 13 tracked exports are not the total PE export count. The two M.1-retired targets (#3 `CELCompiler_ctor`, #4 `CELCompiler_dtor`) were previously resolved by RVA signature; M.1 replaced them with ordinal 66/67 export resolution (see §5 and the RETIRED notes on §4.3/§4.4).

---

### 4.1 Target 1: StartCompilingProcess (RVA 0x165DE0)

| Field | Value |
|---|---|
| **RVA** | 0x165DE0 |
| **VA** | 0x10165DE0 |
| **Name** | StartCompilingProcess |
| **Calling convention** | `__cdecl`, no args |
| **Signature length** | 64 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 51 8B 0D ?? ?? ?? ?? 33 C0 A3 ?? ?? ?? ?? 85 C9 74 15 51 FF 15 ?? ?? ?? ?? A3 ?? ?? ?? ?? 85 C0 75 19 A1 ?? ?? ?? ?? 69
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x00165DE0 (expected RVA). PASS. *(Source: `b1_sig_scan_run.md`, final run, target [1])*

**Structural assumptions:**
- Assumes the standard 24-byte SEH frame prologue (`55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00`), with the SEH handler address (bytes 6–9) wildcarded.
- Assumes `DAT_1023cea0` is the global error-array pointer (accessed via `MOV ECX, [0x1023cea0]` at instruction RVA 0x165DF9 — the 4-byte absolute address is wildcarded).
- Assumes `DAT_1023cea4` is the global error-record count (accessed via `MOV [0x1023cea4], EAX` — wildcarded).
- Assumes error record stride 0x129 (297 bytes) — embedded in the signature as the `69` opcode prefix of `IMUL EAX, EAX, 0x129` (the 0x129 immediate is part of the next instruction, beyond the signature boundary, but the `69` opcode is fixed).
- Distinctive feature: `51 8B 0D` (PUSH ECX; MOV ECX, [imm32]) followed by error array reset and IMUL with 0x129 stride.

---

### 4.2 Target 2: EndCompilingProcess (RVA 0x165ED0)

| Field | Value |
|---|---|
| **RVA** | 0x165ED0 |
| **VA** | 0x10165ED0 |
| **Name** | EndCompilingProcess |
| **Calling convention** | `__fastcall`, 1 arg (int param) |
| **Signature length** | 55 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 51 56 8B 35 ?? ?? ?? ?? 85 F6 74 62 C7 45 FC 00 00 00 00 8B CE 8B 06 FF 70 04 E8 ?? ?? ?? ??
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x00165ED0 (expected RVA). PASS. *(Source: `b1_sig_scan_run.md`, final run, target [2])*

**Structural assumptions:**
- Assumes the standard 24-byte SEH frame prologue (SEH handler at bytes 6–9 wildcarded).
- Assumes `DAT_1023ceb0` is a global pointer to some list/object (accessed via `MOV ESI, [0x1023ceb0]` — wildcarded).
- Assumes the SEH handler address (0x101EC887) is the same as Target 8 (FUN_101a8360) — both functions share the same handler but diverge immediately after the prologue, so the post-prologue bytes distinguish them.
- Distinctive feature: `51 56 8B 35` (PUSH ECX; PUSH ESI; MOV ESI, [imm32]) — differs from Target 1's `51 8B 0D`.

---

### 4.3 Target 3: CELCompiler constructor (RVA 0x165A00)

> **RETIRED 2026-08-24 (M.1).** This signature has been **retired** by
> Workstream M slice M.1 (TSDev parity — compiler-object lifecycle) and is
> **no longer generated** into `signatures/v9.50.01.3344.json` or resolved
> by the production `OrlibResolver`. M.1 replaced direct RVA resolution of
> the CELCompiler constructor with **ordinal-only `[NONAME]` export 66**
> resolution: production code calls `GetProcAddress(m_orlibModule,
> MAKEINTRESOURCEA(66))` (test mode: PE export-table indexing by ordinal).
> Ordinal 66 maps to `FUN_10165f70` (RVA 0x165F70), a `__cdecl` thunk with
> no arguments that returns `void*` and performs `operator_new(0x58)` +
> `ctor FUN_10165a00` (the same constructor this signature targeted, at
> RVA 0x165A00). The historical signature/scan/structural-assumption
> content below is **retained as the historical record** of the pre-M.1
> RVA-resolution approach; it is **superseded** for production purposes by
> the ordinal 66 entry in §5. See `docs/M1_HANDOFF.md` (R-M.1.1); the
> `tmp/m1_ghidra_findings.md` §1 Ghidra export-table evidence is
> historical/transient and unavailable in the tracked checkout. For the
> post-M.1/pre-M.3 and current count phases, see the Historical snapshots
> subsection and Current totals statement above.

| Field | Value |
|---|---|
| **RVA** | 0x165A00 |
| **VA** | 0x10165A00 |
| **Name** | CELCompiler_ctor |
| **Calling convention** | `__fastcall` (ecx=this) |
| **Signature length** | 64 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 08 56 57 8B F9 89 7D F0 8D 47 1C C7 47 04 00 00 00 00 C7 47 08 00 00 00 00 C7 47 0C 00 00 00 00 C7 47 10 00 00 00
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x00165A00 (expected RVA). PASS. *(Source: `b1_sig_scan_run.md`, final run, target [3])*

**Structural assumptions:**
- Assumes the standard 24-byte SEH frame prologue (SEH handler at bytes 6–9 wildcarded).
- Assumes the constructor initializes 6 DWORD fields at `[EDI+4]` through `[EDI+0x18]` to zero (the `C7 47 04/08/0C/10 00 00 00 00` sequence).
- Assumes `LEA EAX, [EDI+0x1C]` (`8D 47 1C`) precedes the field zeroing — part of the constructor's object layout.
- Assumes SSO (small string optimization) capacity = 7 is set at `[EAX+0x14]` (beyond the signature boundary, but the zeroing pattern is distinctive).
- Distinctive feature: `83 EC 08 56 57 8B F9` (SUB ESP,8; PUSH ESI; PUSH EDI; MOV EDI,ECX) — constructor prologue capturing `this` pointer.

---

### 4.4 Target 4: CELCompiler destructor (RVA 0x165AC0)

> **RETIRED 2026-08-24 (M.1).** This signature has been **retired** by
> Workstream M slice M.1 (TSDev parity — compiler-object lifecycle) and is
> **no longer generated** into `signatures/v9.50.01.3344.json` or resolved
> by the production `OrlibResolver`. M.1 replaced direct RVA resolution of
> the CELCompiler destructor with **ordinal-only `[NONAME]` export 67**
> resolution: production code calls `GetProcAddress(m_orlibModule,
> MAKEINTRESOURCEA(67))` (test mode: PE export-table indexing by ordinal).
> Ordinal 67 maps to `FUN_10165fd0` (RVA 0x165FD0), a `__stdcall` function
> taking one argument `void* p` and returning void, which performs
> `dtor FUN_10165ac0` (the same destructor this signature targeted, at
> RVA 0x165AC0) + `operator delete(p, 0x58)`. Ordinal 67 frees the object
> internally; callers must **not** double-free. The historical
> signature/scan/structural-assumption content below is **retained as the
> historical record** of the pre-M.1 RVA-resolution approach; it is
> **superseded** for production purposes by the ordinal 67 entry in §5.
> See `docs/M1_HANDOFF.md` (R-M.1.1); the `tmp/m1_ghidra_findings.md` §1
> Ghidra export-table evidence is historical/transient and unavailable in the
> tracked checkout. For the post-M.1/pre-M.3 and current count phases, see
> the Historical snapshots subsection and Current totals statement above.

| Field | Value |
|---|---|
| **RVA** | 0x165AC0 |
| **VA** | 0x10165AC0 |
| **Name** | CELCompiler_dtor |
| **Calling convention** | `__fastcall` (ecx=this) |
| **Signature length** | 38 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 51 56 57 8B F9 8B 47 4C 8D 4F 4C 50 FF 30 8D 45 FC 50 E8 ?? ?? ?? ?? 6A 20 6A 01 FF 77 4C E8 ?? ?? ?? ??
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x00165AC0 (expected RVA). PASS. *(Source: `b1_sig_scan_run.md`, final run, target [4])*

**Structural assumptions:**
- Assumes **NO SEH frame** — this function uses a minimal prologue (`55 8B EC 51 56 57 8B F9`), distinct from the 24-byte SEH prologue used by most other functions.
- Assumes access to `[EDI+0x4C]` (destructor cleanup of some field — `MOV EAX, [EDI+0x4C]` and `LEA ECX, [EDI+0x4C]`).
- Assumes two relative calls (`E8 ?? ?? ?? ??`) to cleanup helper functions — wildcarded.
- Distinctive feature: `55 8B EC 51 56 57 8B F9 8B 47 4C 8D 4F 4C` — very distinctive (no SEH frame; PUSH ECX; PUSH ESI; PUSH EDI; MOV EDI,ECX; MOV EAX,[EDI+0x4C]; LEA ECX,[EDI+0x4C]).

---

### 4.5 Target 5: InitGlobalLookup (RVA 0x1A9460)

| Field | Value |
|---|---|
| **RVA** | 0x1A9460 |
| **VA** | 0x101A9460 |
| **Name** | InitGlobalLookup |
| **Calling convention** | `__cdecl`, no args |
| **Signature length** | 66 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 51 A1 ?? ?? ?? ?? 85 C0 75 3A 6A 2C FF 15 ?? ?? ?? ?? 83 C4 04 89 45 F0 C7 45 FC 00 00 00 00 85 C0 74 1A 8B C8 E8 ?? ?? ?? ??
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001A9460 (expected RVA). PASS. *(Source: `b1_sig_scan_run.md`, final run, target [5])*

**Structural assumptions:**
- Assumes the standard 24-byte SEH frame prologue (SEH handler at bytes 6–9 wildcarded).
- Assumes `DAT_1023ced4` is a global singleton pointer for the lookup object (accessed via `MOV EAX, [0x1023ced4]` — wildcarded).
- Assumes allocation of 0x2C (44) bytes for the lookup object (`6A 2C` = PUSH 0x2C before IAT call).
- Assumes the IAT indirect call (`FF 15 ?? ?? ?? ??`) is to an allocator (e.g., `malloc`/`operator new`).
- Distinctive feature: `51 A1` (PUSH ECX; MOV EAX, [imm32]) — accesses DAT_1023ced4, allocates 0x2C bytes.

---

### 4.6 Target 6: DeleteFromTree (RVA 0x1E4030)

| Field | Value |
|---|---|
| **RVA** | 0x1E4030 |
| **VA** | 0x101E4030 |
| **Name** | DeleteFromTree |
| **Calling convention** | `__thiscall` (ecx=globalLookup; stack: whType, progId, flag); RET 12 |
| **Signature length** | 66 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 28 83 7D 10 00 53 56 8B 75 0C 8B D9 57 89 5D F0 74 0C 56 FF 75 08 E8 ?? ?? ?? ?? 83 C4 08 8D 43 08 50 FF 15 ?? ?? ?? ??
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001E4030 (expected RVA). PASS. *(Source: `b1_sig_scan_run.md`, final run, target [6])*

**Structural assumptions:**
- Assumes the standard 24-byte SEH frame prologue (SEH handler at bytes 6–9 wildcarded).
- Assumes `__thiscall` convention: ECX = `globalLookup` object pointer; 3 stack args at `[ebp+0x8]` (whType), `[ebp+0xc]` (progId), `[ebp+0x10]` (flag).
- Assumes the flag at `[ebp+0x10]` controls a conditional path (`CMP [EBP+0x10], 0` → `JZ`).
- Assumes a relative call (`E8 ?? ?? ?? ??`) to a helper function when flag != 0, and an IAT indirect call (`FF 15 ?? ?? ?? ??`) for tree manipulation.
- Distinctive feature: `83 EC 28 83 7D 10 00 53 56 8B 75 0C 8B D9 57` — SUB ESP,0x28; CMP [EBP+0x10],0; PUSH EBX; PUSH ESI; MOV ESI,[EBP+0xc]; MOV EBX,ECX; PUSH EDI.

---

### 4.7 Target 7: GetWarehouse (RVA 0x1A9650)

| Field | Value |
|---|---|
| **RVA** | 0x1A9650 |
| **VA** | 0x101A9650 |
| **Name** | GetWarehouse |
| **Calling convention** | `__cdecl`, 1 arg (path; 0=default) |
| **Signature length** | 45 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA). Note: resolved and stored (`m_pGetWarehouse`) but NOT actually called in the production flow — the delete orchestrator (FUN_101a8360) calls it internally. Retained as a resolved-but-unused CALL_TARGET. |

**Signature:**
```
55 8B EC 64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 81 EC 94 01 00 00 A1 ?? ?? ?? ?? 53 56 57 A8 01 75 66 83 C8 01
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001A9650 (expected RVA). PASS. *(Source: `b1_sig_scan_run.md`, final run, target [7])*

**Structural assumptions:**
- Assumes a **DIFFERENT SEH prologue order**: `MOV EAX, FS:[0]` (`64 A1 00 00 00 00`) occurs **BEFORE** `PUSH -1` / `PUSH handler` (`6A FF 68 ?? ?? ?? ??`). This is unique among the original 12 targets. The SEH handler address is at bytes 13–16 (wildcarded), not 6–9.
- Assumes `DAT_102408f8` is a one-time-init guard flag (accessed via `MOV EAX, [0x102408f8]` — wildcarded; `TEST AL, 0x1` checks if already initialized).
- Assumes a 0x194 (404) byte stack frame (`81 EC 94 01 00 00` = SUB ESP, 0x194).
- Distinctive feature: `55 8B EC 64 A1` (MOV EAX,FS:[0] before PUSH -1) + `81 EC 94 01 00 00` (large frame) + `A8 01` (TEST AL,1 — one-time-init guard).

---

### 4.8 Target 8: FUN_101a8360 delete orchestrator (RVA 0x1A8360)

| Field | Value |
|---|---|
| **RVA** | 0x1A8360 |
| **VA** | 0x101A8360 |
| **Name** | FUN_101a8360_delete_orchestrator |
| **Calling convention** | `__fastcall` (ECX=whType, EDX=progId, no stack args); invoked via inline asm |
| **Signature length** | 64 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 81 EC DC 02 00 00 53 56 8B F2 8B D9 57 8D 4D CC 89 75 EC E8 ?? ?? ?? ?? 6A 00 E8 ?? ?? ?? ?? 83 C4 04 8B F8 8D 45 90 89
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001A8360 (expected RVA). PASS. *(Source: `b1_sig_scan_run.md`, final run, target [8])*

**Structural assumptions:**
- Assumes the standard 24-byte SEH frame prologue (SEH handler at bytes 6–9 wildcarded).
- Assumes `__fastcall` convention: ECX = whType, EDX = progId (captured via `MOV ESI,EDX` / `MOV EBX,ECX`).
- Assumes a 0x2DC (732) byte stack frame (`81 EC DC 02 00 00` = SUB ESP, 0x2DC) — very large frame.
- Assumes the SEH handler address (0x101EC887) is the same as Target 2 (EndCompilingProcess) — both functions share the same handler but diverge immediately after the prologue.
- Assumes a relative call to GetWarehouse (`E8 ?? ?? ?? ??` after `6A 00` = PUSH 0) — the call target is wildcarded.
- Distinctive feature: `81 EC DC 02 00 00` (732-byte frame) + `8B F2 8B D9` (MOV ESI,EDX; MOV EBX,ECX — fastcall arg capture).

---

### 4.9 Target 9: RETIRED (FUN_10011230 persistence patch) — removed in TVS-L.3

> **RETIRED in TVS-L.3.** The `FUN_10011230_persistence_patch` entry (target
> #9) has been **completely removed** from this catalog as part of TVS-L.3
> (`docs/workstream_unknown_link_fix_plan.md`). The persistence bypass patch
> is no longer applied in production code; the real finalization path
> (`FUN_10011230` → `FUN_1008E640` → `SymInfo::AdjustOffsets` →
> `SymWriter::Save`, then `ModuleInfo::SetGuid` / `MDVault::SaveModule`) now
> runs. Target #9 is retained as a **stable-ID gap** — targets 1–8 and 10–25
> keep their original IDs; generated array indices must not be inferred from
> IDs. The historical 2026-08-23 supersession content that previously occupied
> this section (calling-convention correction: caller `ADD ESP,0x0C` at
> `0x1001E0C1`, original plain `RET` at `0x10011454`, patched `RET 0x0C` as
> callee-cleanup ABI hazard; patch-details correction: bypass that produced
> runtime-unlinkable output) is preserved in the top-of-file banner and in
> `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`.

---

### 4.10 Target 10: FUN_101d0950 thread-check INT3 patch (RVA 0x1D0993)

| Field | Value |
|---|---|
| **RVA (patch site)** | 0x1D0993 |
| **VA (patch site)** | 0x101D0993 |
| **Containing function** | FUN_101d0950 (RVA 0x1D0950, VA 0x101D0950) |
| **Name** | FUN_101d0950_thread_check_INT3 |
| **Calling convention** | `__thiscall` (ecx=this) |
| **Signature length** | 36 bytes |
| **Patch site?** | Yes |
| **Sig match RVA** | 0x1D0950 (containing function entry) |
| **Patch offset** | 0x43 (67) from function entry |
| **Original byte at patch site** | `CC` (INT3) |
| **Replacement byte** | `90` (NOP) |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature (for the containing function FUN_101d0950):**
```
55 8B EC 56 8B F1 83 BE AE 00 00 00 00 0F 84 ?? ?? ?? ?? FF 75 08 B8 00 20 00 00 66 89 46 1E E8 ?? ?? ?? ??
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001D0950 (containing function entry). PASS. Patch byte check: OK (byte at offset 0x43 = 0xCC, matches original). *(Source: `b1_sig_scan_run.md`, final run, target [10])*

**Patch details:**
- Single-byte patch: `0xCC` (INT3) → `0x90` (NOP) at function entry + 0x43.
- This NOP's the thread-check INT3 in the command 0x0F (load program) dispatch helper, allowing it to be called from any thread.
- Context bytes at patch site: `3B 46 76 74 01 CC FF 76 6E C7 46 04 0F 00 00 00` (CMP EAX,[ESI+0x76]; JZ +1; INT3; PUSH [ESI+0x6e]; MOV [ESI+4], 0x0F).
- The production code verifies the byte is either `0xCC` or `0x90` before patching; skips idempotently if already `0x90`.

**Structural assumptions:**
- Assumes `__thiscall` convention (ecx=this) — `8B F1` (MOV ESI,ECX) captures the this pointer.
- Assumes **NO SEH frame** — minimal prologue (`55 8B EC 56 8B F1`).
- Assumes `[ESI+0xAE]` field check (`83 BE AE 00 00 00 00` = CMP [ESI+0xAE], 0 — 7-byte encoding with 32-bit displacement) controls the command dispatch path.
- Assumes `MOV EAX, 0x2000` (`B8 00 20 00 00`) sets a window style or flag.
- Assumes the INT3 at offset 0x43 is the thread-check breakpoint (command 0x0F — load program).

---

### 4.11 Target 11: FUN_101cf5b0 thread-check INT3 patch (RVA 0x1CF5D4)

| Field | Value |
|---|---|
| **RVA (patch site)** | 0x1CF5D4 |
| **VA (patch site)** | 0x101CF5D4 |
| **Containing function** | FUN_101cf5b0 (RVA 0x1CF5B0, VA 0x101CF5B0) |
| **Name** | FUN_101cf5b0_thread_check_INT3 |
| **Calling convention** | `__thiscall` (ecx=this) |
| **Signature length** | 37 bytes |
| **Patch site?** | Yes |
| **Sig match RVA** | 0x1CF5B0 (containing function entry) |
| **Patch offset** | 0x24 (36) from function entry |
| **Original byte at patch site** | `CC` (INT3) |
| **Replacement byte** | `90` (NOP) |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature (for the containing function FUN_101cf5b0):**
```
55 8B EC 56 8B F1 6A FF FF 76 72 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 46 76 8B 45 08 74 06 83 F8 1E 74 01 CC
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001CF5B0 (containing function entry). PASS. Patch byte check: OK (byte at offset 0x24 = 0xCC, matches original). *(Source: `b1_sig_scan_run.md`, final run, target [11])*

**Patch details:**
- Single-byte patch: `0xCC` (INT3) → `0x90` (NOP) at function entry + 0x24.
- This NOP's the thread-check INT3 in the general dispatch helper, allowing it to be called from any thread.
- Context bytes at patch site: `83 F8 1E 74 01 CC FF 76 6E 89 46 04 FF 15 ?? ?? ?? ??` (CMP EAX,0x1E; JZ +1; INT3; PUSH [ESI+0x6e]; MOV [ESI+4], EAX; CALL [IAT]).
- The production code verifies the byte is either `0xCC` or `0x90` before patching; skips idempotently if already `0x90`.

**Structural assumptions:**
- Assumes `__thiscall` convention (ecx=this) — `8B F1` (MOV ESI,ECX) captures the this pointer.
- Assumes **NO SEH frame** — minimal prologue (`55 8B EC 56 8B F1`).
- Assumes `PUSH [ESI+0x72]` (`FF 76 72`) accesses a field before two IAT indirect calls.
- Assumes `CMP EAX, 0x1E` (`83 F8 1E`) checks for command 0x1E before the thread-check INT3.
- Assumes the INT3 at offset 0x24 is the thread-check breakpoint (general dispatch helper).

---

### 4.12 Target 12: FUN_101d1fa0 thread-check INT3 patch (RVA 0x1D1FC8)

| Field | Value |
|---|---|
| **RVA (patch site)** | 0x1D1FC8 |
| **VA (patch site)** | 0x101D1FC8 |
| **Containing function** | FUN_101d1fa0 (RVA 0x1D1FA0, VA 0x101D1FA0) |
| **Name** | FUN_101d1fa0_thread_check_INT3 |
| **Calling convention** | `__thiscall` (ecx=this) |
| **Signature length** | 57 bytes |
| **Patch site?** | Yes |
| **Sig match RVA** | 0x1D1FA0 (containing function entry) |
| **Patch offset** | 0x28 (40) from function entry |
| **Original byte at patch site** | `CC` (INT3) |
| **Replacement byte** | `90` (NOP) |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature (for the containing function FUN_101d1fa0):**
```
55 8B EC 8B 45 08 56 8B F1 6A FF FF 76 72 89 46 12 8B 45 0C 89 46 16 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 17 00 00 00 FF 15 ?? ?? ?? ??
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001D1FA0 (containing function entry). PASS. Patch byte check: OK (byte at offset 0x28 = 0xCC, matches original). *(Source: `b1_sig_scan_run.md`, final run, target [12])*

**Note on signature evolution:** This signature was initially 41 bytes and matched twice in `.text` (a spurious match at RVA 0x001CFCE0 in addition to the expected 0x001D1FA0). The signature was extended to 57 bytes by appending the post-INT3 context (`FF 76 6E C7 46 04 17 00 00 00 FF 15 ?? ?? ?? ??`), which disambiguated the duplicate. The final scan confirms uniqueness (count = 1). *(Source: `b1_sig_scan_run.md`, §Comparison with previous run)*

**Patch details:**
- Single-byte patch: `0xCC` (INT3) → `0x90` (NOP) at function entry + 0x28.
- This NOP's the thread-check INT3 in the command 0x17 (save/commit) dispatch helper, allowing it to be called from any thread.
- Context bytes at patch site: `3B 46 76 74 01 CC FF 76 6E C7 46 04 17 00 00 00 FF 15 ?? ?? ?? ??` (CMP EAX,[ESI+0x76]; JZ +1; INT3; PUSH [ESI+0x6e]; MOV [ESI+4], 0x17; CALL [IAT]).
- The production code verifies the byte is either `0xCC` or `0x90` before patching; skips idempotently if already `0x90`.

**Structural assumptions:**
- Assumes `__thiscall` convention (ecx=this) — `8B F1` (MOV ESI,ECX) captures the this pointer.
- Assumes **NO SEH frame** — minimal prologue (`55 8B EC 8B 45 08 56 8B F1`).
- Assumes both stack args are read into struct fields before the thread check: `[EBP+0x8]` → `[ESI+0x12]` (`89 46 12`) and `[EBP+0xc]` → `[ESI+0x16]` (`89 46 16`).
- Assumes `PUSH [ESI+0x72]` (`FF 76 72`) — same pattern as Target 11.
- Assumes command 0x17 (save/commit) is embedded as `C7 46 04 17 00 00 00` (MOV [ESI+4], 0x17) — this immediate distinguishes Target 12 from Target 10 (which uses 0x0F).
- Assumes the INT3 at offset 0x28 is the thread-check breakpoint (command 0x17 — save/commit).

---

### 4.13 Target 13: FUN_101d0e80 thread-check INT3 patch (RVA 0x1D0EB9)

| Field | Value |
|---|---|
| **RVA (patch site)** | 0x1D0EB9 |
| **VA (patch site)** | 0x101D0EB9 |
| **Containing function** | FUN_101d0e80 (RVA 0x1D0E80, VA 0x101D0E80) |
| **Name** | FUN_101d0e80_thread_check_INT3 |
| **Calling convention** | `__thiscall` (ecx=this) |
| **Signature length** | 74 bytes |
| **Patch site?** | Yes |
| **Sig match RVA** | 0x1D0E80 (containing function entry) |
| **Patch offset** | 0x39 (57) from function entry |
| **Original byte at patch site** | `CC` (INT3) |
| **Replacement byte** | `90` (NOP) |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature (for the containing function FUN_101d0e80):**
```
55 8B EC 51 8B 45 08 53 8B 1D ?? ?? ?? ?? 56 8B 75 10 57 8B F9 6A FF C7 06 00 00 00 00 89 7D FC FF 77 72 89 47 12 8B 45 0C 89 47 16 FF D3 FF 15 ?? ?? ?? ?? 3B 47 76 74 01 CC FF 77 6E C7 47 04 11 00 00 00 FF 15 ?? ?? ?? ??
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001D0E80 (containing function entry). PASS. *(Source: `signatures/v9.50.01.3344.json`, expectedMatchCount=1)*

**Patch details:**
- Single-byte patch: `0xCC` (INT3) → `0x90` (NOP) at function entry + 0x39.
- This NOP's the thread-check INT3 in the command 0x11 (source-text load) dispatch helper, allowing it to be called from any thread.
- Context bytes at patch site: `3B 47 76 74 01 CC FF 77 6E C7 47 04 11 00 00 00` (CMP EAX,[EDI+0x76]; JZ +1; INT3; PUSH [EDI+0x6E]; MOV [EDI+4], 0x11).
- The production code verifies the byte is either `0xCC` or `0x90` before patching; skips idempotently if already `0x90`.

**Structural assumptions:**
- Assumes `__thiscall` convention (ecx=this) — `8B F9` (MOV EDI,ECX) captures the this pointer.
- Assumes `MOV EBX, [imm32]` (`8B 1D ?? ?? ?? ??`) loads a function pointer (wildcarded IAT/data reference).
- Assumes both stack args are read into struct fields before the thread check: `[EBP+0x8]` → `[EDI+0x12]` (`89 47 12`) and `[EBP+0xc]` → `[EDI+0x16]` (`89 47 16`).
- Assumes command 0x11 (source-text load) is embedded as `C7 47 04 11 00 00 00` (MOV [EDI+4], 0x11).
- Assumes the INT3 at offset 0x39 is the thread-check breakpoint (command 0x11).
- *Provenance: RE-F4 (2026-08-19), `docs/RE/RE_F4_cmd11_patch_findings.md`*

---

### 4.14 Target 14: FUN_101d0c30 thread-check INT3 patch (RVA 0x1D0C58)

| Field | Value |
|---|---|
| **RVA (patch site)** | 0x1D0C58 |
| **VA (patch site)** | 0x101D0C58 |
| **Containing function** | FUN_101d0c30 (RVA 0x1D0C30, VA 0x101D0C30) |
| **Name** | FUN_101d0c30_thread_check_INT3 |
| **Calling convention** | `__thiscall` (ecx=this) |
| **Signature length** | 51 bytes |
| **Patch site?** | Yes |
| **Sig match RVA** | 0x1D0C30 (containing function entry) |
| **Patch offset** | 0x28 (40) from function entry |
| **Original byte at patch site** | `CC` (INT3) |
| **Replacement byte** | `90` (NOP) |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature (for the containing function FUN_101d0c30):**
```
55 8B EC 8B 45 08 83 EC 08 56 57 8B 3D ?? ?? ?? ?? 8B F1 6A FF FF 76 72 89 46 12 FF D7 FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 08 00 00 00
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001D0C30 (containing function entry). PASS. *(Source: `signatures/v9.50.01.3344.json`, expectedMatchCount=1)*

**Patch details:**
- Single-byte patch: `0xCC` (INT3) → `0x90` (NOP) at function entry + 0x28.
- This NOP's the thread-check INT3 in the command 0x08 (type select) dispatch helper, allowing it to be called from any thread.
- Context bytes at patch site: `3B 46 76 74 01 CC FF 76 6E C7 46 04 08 00 00 00` (CMP EAX,[ESI+0x76]; JZ +1; INT3; PUSH [ESI+0x6E]; MOV [ESI+4], 0x08).
- The production code verifies the byte is either `0xCC` or `0x90` before patching; skips idempotently if already `0x90`.

**Structural assumptions:**
- Assumes `__thiscall` convention (ecx=this) — `8B F1` (MOV ESI,ECX) captures the this pointer.
- Assumes `MOV EDI, [imm32]` (`8B 3D ?? ?? ?? ??`) loads a function pointer (wildcarded).
- Assumes `[EBP+0x8]` → `[ESI+0x12]` (`89 46 12`) stores the first stack arg.
- Assumes command 0x08 (type select) is embedded as `C7 46 04 08 00 00 00` (MOV [ESI+4], 0x08).
- Assumes the INT3 at offset 0x28 is the thread-check breakpoint (command 0x08).
- *Provenance: RE-F5 (2026-08-20), `docs/RE/RE_F5_enumeration_findings.md`*

---

### 4.15 Target 15: FUN_101d16c0 thread-check INT3 patch (RVA 0x1D1714)

| Field | Value |
|---|---|
| **RVA (patch site)** | 0x1D1714 |
| **VA (patch site)** | 0x101D1714 |
| **Containing function** | FUN_101d16c0 (RVA 0x1D16C0, VA 0x101D16C0) |
| **Name** | FUN_101d16c0_thread_check_INT3 |
| **Calling convention** | `__thiscall` (ecx=this) |
| **Signature length** | 132 bytes |
| **Patch site?** | Yes |
| **Sig match RVA** | 0x1D16C0 (containing function entry) |
| **Patch offset** | 0x54 (84) from function entry |
| **Original byte at patch site** | `CC` (INT3) |
| **Replacement byte** | `90` (NOP) |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature (for the containing function FUN_101d16c0):**
```
55 8B EC 64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 56 8B F1 57 83 BE AE 00 00 00 00 0F 84 ?? ?? ?? ?? 8B 7D 10 8B 45 08 6A FF C7 07 00 00 00 00 FF 76 72 89 46 12 8B 45 0C 89 46 08 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 09 00 00 00 FF 15 ?? ?? ?? ?? 6A FF FF 76 6A FF 15 ?? ?? ?? ?? FF 76 72 FF 15 ?? ?? ?? ?? 83 7E 5E 00 74 ?? 68 41 02 00 00
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001D16C0 (containing function entry). PASS. *(Source: `signatures/v9.50.01.3344.json`, expectedMatchCount=1)*

**Patch details:**
- Single-byte patch: `0xCC` (INT3) → `0x90` (NOP) at function entry + 0x54.
- This NOP's the thread-check INT3 in the command 0x09 (item retrieval) dispatch helper, allowing it to be called from any thread.
- Context bytes at patch site: `3B 46 76 74 01 CC FF 76 6E C7 46 04 09 00 00 00` (CMP EAX,[ESI+0x76]; JZ +1; INT3; PUSH [ESI+0x6E]; MOV [ESI+4], 0x09).
- The production code verifies the byte is either `0xCC` or `0x90` before patching; skips idempotently if already `0x90`.

**Structural assumptions:**
- Assumes the standard 24-byte SEH frame prologue (SEH handler at bytes 13–16 wildcarded — note: `MOV EAX, FS:[0]` before `PUSH -1`, same as Target 7 GetWarehouse).
- Assumes `__thiscall` convention (ecx=this) — `8B F1` (MOV ESI,ECX) captures the this pointer.
- Assumes `[ESI+0xAE]` field check (`83 BE AE 00 00 00 00`) controls a conditional branch.
- Assumes `[EBP+0x10]` → `EDI` (`8B 7D 10`), `[EBP+0x8]` → `[ESI+0x12]` (`89 46 12`), `[EBP+0xc]` → `[ESI+0x08]` (`89 46 08`).
- Assumes command 0x09 (item retrieval) is embedded as `C7 46 04 09 00 00 00` (MOV [ESI+4], 0x09).
- Assumes the INT3 at offset 0x54 is the thread-check breakpoint (command 0x09).
- The 132-byte signature is the longest in the catalog, capturing the full dispatch path including post-INT3 calls.
- *Provenance: RE-F5 (2026-08-20), `docs/RE/RE_F5_enumeration_findings.md`*

---

### 4.16 Target 16: FUN_101d0d90 thread-check INT3 patch (RVA 0x1D0DBF)

| Field | Value |
|---|---|
| **RVA (patch site)** | 0x1D0DBF |
| **VA (patch site)** | 0x101D0DBF |
| **Containing function** | FUN_101d0d90 (RVA 0x1D0D90, VA 0x101D0D90) |
| **Name** | FUN_101d0d90_thread_check_INT3 |
| **Calling convention** | `__thiscall` (ecx=this) |
| **Signature length** | 58 bytes |
| **Patch site?** | Yes |
| **Sig match RVA** | 0x1D0D90 (containing function entry) |
| **Patch offset** | 0x2F (47) from function entry |
| **Original byte at patch site** | `CC` (INT3) |
| **Replacement byte** | `90` (NOP) |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature (for the containing function FUN_101d0d90):**
```
55 8B EC 8B 45 08 53 56 57 33 FF 33 DB 83 38 FF 8B F1 74 50 8B 00 6A FF FF 76 72 89 46 12 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 07 00 00 00
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001D0D90 (containing function entry). PASS. *(Source: `signatures/v9.50.01.3344.json`, expectedMatchCount=1)*

**Patch details:**
- Single-byte patch: `0xCC` (INT3) → `0x90` (NOP) at function entry + 0x2F.
- This NOP's the thread-check INT3 in the command 0x07 (count by types) dispatch helper, allowing it to be called from any thread.
- Context bytes at patch site: `3B 46 76 74 01 CC FF 76 6E C7 46 04 07 00 00 00` (CMP EAX,[ESI+0x76]; JZ +1; INT3; PUSH [ESI+0x6E]; MOV [ESI+4], 0x07).
- The production code verifies the byte is either `0xCC` or `0x90` before patching; skips idempotently if already `0x90`.

**Structural assumptions:**
- Assumes `__thiscall` convention (ecx=this) — `8B F1` (MOV ESI,ECX) captures the this pointer.
- Assumes **NO SEH frame** — minimal prologue (`55 8B EC 8B 45 08 53 56 57`).
- Assumes `33 FF 33 DB` (XOR EDI,EDI; XOR EBX,EBX) zeroes two registers before `CMP [EAX], -1` (`83 38 FF`).
- Assumes `[EBP+0x8]` → `[ESI+0x12]` (`89 46 12`) stores the first stack arg.
- Assumes command 0x07 (count by types) is embedded as `C7 46 04 07 00 00 00` (MOV [ESI+4], 0x07).
- Assumes the INT3 at offset 0x2F is the thread-check breakpoint (command 0x07).
- *Provenance: RE-F5 (2026-08-20), `docs/RE/RE_F5_enumeration_findings.md`*

---

### 4.17 Target 17: FUN_101d43a0 build type array (RVA 0x1D43A0)

| Field | Value |
|---|---|
| **RVA** | 0x1D43A0 |
| **VA** | 0x101D43A0 |
| **Name** | FUN_101d43a0_build_type_array |
| **Calling convention** | `__stdcall` (2 stack args; RET 8) |
| **Signature length** | 48 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 8B 45 08 83 F8 04 0F 87 B8 00 00 00 FF 24 85 ?? ?? ?? ?? 8B 45 0C C7 00 00 00 00 00 C7 40 04 00 02 00 00 C7 40 08 FF FF FF FF 5D C2 08
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001D43A0 (expected RVA). PASS. *(Source: `signatures/v9.50.01.3344.json`, expectedMatchCount=1)*

**Structural assumptions:**
- Assumes `__stdcall` convention with 2 stack args (`[EBP+0x8]` = type filter, `[EBP+0xC]` = output array pointer); `RET 8` (`C2 08`).
- Assumes `CMP EAX, 4` (`83 F8 04`) checks the warehouse type against 4 (Function).
- Assumes `JMP [EAX*4 + imm32]` (`FF 24 85 ?? ?? ?? ??`) is a switch-table jump (wildcarded table address).
- Assumes the default case (type > 4) zeroes the output array and sets sentinel values: `C7 00 00 00 00 00` (zero count), `C7 40 04 00 02 00 00` (0x200 capacity), `C7 40 08 FF FF FF FF` (0xFFFFFFFF sentinel).
- Distinctive feature: `8B 45 08 83 F8 04` (MOV EAX,[EBP+8]; CMP EAX,4) + `FF 24 85` (JMP [EAX*4+...]) — switch-table type-array builder.
- *Provenance: RE-F5 (2026-08-20), `docs/RE/RE_F5_warehouse_com_interface.md §8`*

---

### 4.18 Target 18: FUN_1000dd30 CELProgramWriter constructor (RVA 0xDD30)

| Field | Value |
|---|---|
| **RVA** | 0xDD30 |
| **VA** | 0x1000DD30 |
| **Name** | FUN_1000dd30_celprogwriter_ctor |
| **Calling convention** | `__fastcall` (ecx=this) |
| **Signature length** | 63 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 51 56 8B F1 89 75 FC E8 C1 1D 1B 00 C7 06 ?? ?? ?? ?? 8B C6 C7 86 41 02 00 00 01 00 00 00 C7 86 45 02 00 00 00 00 00 00 C7 86 49 02 00 00 00 00 00 00 C7 86 4D 02 00 00 00 00 00 00
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x0000DD30 (expected RVA). PASS. *(Source: `signatures/v9.50.01.3344.json`, expectedMatchCount=1)*

**Structural assumptions:**
- Assumes `__fastcall` convention (ecx=this) — `8B F1` (MOV ESI,ECX) captures the this pointer.
- Assumes **NO SEH frame** — minimal prologue (`55 8B EC 51 56 8B F1`).
- Assumes a relative call (`E8 C1 1D 1B 00`) to a base constructor — this is a FIXED (non-wildcarded) relative displacement, unusual in this catalog. It works because the call target is at a fixed offset from the signature.
- Assumes `MOV [ESI], vftable` (`C7 06 ?? ?? ?? ??`) sets the vtable pointer (wildcarded vtable address).
- Assumes initialization of fields at `[ESI+0x241]` = 1, `[ESI+0x245]` = 0, `[ESI+0x249]` = 0, `[ESI+0x24D]` = 0 (`C7 86 41/45/49/4D 02 00 00`).
- Distinctive feature: `51 56 8B F1` (push ecx; push esi; mov esi,ecx) + `C7 06` (MOV [ESI],vftable) + `C7 86 41 02` (MOV [ESI+0x241],1) — CELProgramWriter constructor.
- *Provenance: RE-F5 (2026-08-20), `docs/RE/RE_F5_warehouse_com_interface.md §8`*

---

### 4.19 Target 19: FUN_101cfce0 thread-check INT3 patch (RVA 0x1CFD08)

| Field | Value |
|---|---|
| **RVA (patch site)** | 0x1CFD08 |
| **VA (patch site)** | 0x101CFD08 |
| **Containing function** | FUN_101cfce0 (RVA 0x1CFCE0, VA 0x101CFCE0) |
| **Name** | FUN_101cfce0_thread_check_INT3 |
| **Calling convention** | `__thiscall` (ecx=this) |
| **Signature length** | 51 bytes |
| **Patch site?** | Yes |
| **Sig match RVA** | 0x1CFCE0 (containing function entry) |
| **Patch offset** | 0x28 (40) from function entry |
| **Original byte at patch site** | `CC` (INT3) |
| **Replacement byte** | `90` (NOP) |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature (for the containing function FUN_101cfce0):**
```
55 8B EC 8B 45 08 56 8B F1 6A FF FF 76 72 89 46 12 8B 45 0C 89 46 16 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 0D 00 00 00
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001CFCE0 (containing function entry). PASS. *(Source: `signatures/v9.50.01.3344.json`, expectedMatchCount=1)*

**Patch details:**
- Single-byte patch: `0xCC` (INT3) → `0x90` (NOP) at function entry + 0x28.
- This NOP's the thread-check INT3 in the command 0x0D (COM interface get) dispatch helper, allowing it to be called from any thread.
- Context bytes at patch site: `3B 46 76 74 01 CC FF 76 6E C7 46 04 0D 00 00 00` (CMP EAX,[ESI+0x76]; JZ +1; INT3; PUSH [ESI+0x6E]; MOV [ESI+4], 0x0D).
- The production code verifies the byte is either `0xCC` or `0x90` before patching; skips idempotently if already `0x90`.

**Structural assumptions:**
- Assumes `__thiscall` convention (ecx=this) — `8B F1` (MOV ESI,ECX) captures the this pointer.
- Assumes **NO SEH frame** — minimal prologue (`55 8B EC 8B 45 08 56 8B F1`).
- Assumes both stack args are read into struct fields: `[EBP+0x8]` → `[ESI+0x12]` (`89 46 12`), `[EBP+0xc]` → `[ESI+0x16]` (`89 46 16`).
- Assumes command 0x0D (COM interface get) is embedded as `C7 46 04 0D 00 00 00` (MOV [ESI+4], 0x0D) — distinguishes from FUN_101d1fa0 which uses cmd 0x17.
- Assumes the INT3 at offset 0x28 is the thread-check breakpoint (command 0x0D).
- *Provenance: RE-F5 (2026-08-20), `docs/RE/archive/RE_F5_cache_refresh_feasibility.md`*

---

### 4.20 Target 20: FUN_101d15c0 thread-check INT3 patch (RVA 0x1D1614)

| Field | Value |
|---|---|
| **RVA (patch site)** | 0x1D1614 |
| **VA (patch site)** | 0x101D1614 |
| **Containing function** | FUN_101d15c0 (RVA 0x1D15C0, VA 0x101D15C0) |
| **Name** | FUN_101d15c0_thread_check_INT3 |
| **Calling convention** | `__thiscall` (ecx=this) |
| **Signature length** | 95 bytes |
| **Patch site?** | Yes |
| **Sig match RVA** | 0x1D15C0 (containing function entry) |
| **Patch offset** | 0x54 (84) from function entry |
| **Original byte at patch site** | `CC` (INT3) |
| **Replacement byte** | `90` (NOP) |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature (for the containing function FUN_101d15c0):**
```
55 8B EC 64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 53 56 8B F1 83 BE AE 00 00 00 00 0F 84 ?? ?? ?? ?? 8B 5D 10 8B 45 08 6A FF C7 03 00 00 00 00 FF 76 72 89 46 12 8B 45 0C 89 46 16 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 0D 00 00 00
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001D15C0 (containing function entry). PASS. *(Source: `signatures/v9.50.01.3344.json`, expectedMatchCount=1)*

**Patch details:**
- Single-byte patch: `0xCC` (INT3) → `0x90` (NOP) at function entry + 0x54.
- This NOP's the thread-check INT3 in the command 0x0D (get record by type+index) local-mode dispatch helper, allowing it to be called from any thread.
- Context bytes at patch site: `3B 46 76 74 01 CC FF 76 6E C7 46 04 0D 00 00 00` (CMP EAX,[ESI+0x76]; JZ +1; INT3; PUSH [ESI+0x6E]; MOV [ESI+4], 0x0D).
- The production code verifies the byte is either `0xCC` or `0x90` before patching; skips idempotently if already `0x90`.

**Structural assumptions:**
- Assumes the standard 24-byte SEH frame prologue (SEH handler at bytes 13–16 wildcarded — `MOV EAX, FS:[0]` before `PUSH -1`, same as Target 7/15).
- Assumes `__thiscall` convention (ecx=this) — `8B F1` (MOV ESI,ECX) captures the this pointer.
- Assumes `[ESI+0xAE]` field check (`83 BE AE 00 00 00 00`) controls a conditional branch.
- Assumes `MOV EBX, [EBP+0x10]` (`8B 5D 10`) captures the third stack arg, then `MOV [EBX], 0` (`C7 03 00 00 00 00`).
- Assumes both stack args are read into struct fields: `[EBP+0x8]` → `[ESI+0x12]` (`89 46 12`), `[EBP+0xc]` → `[ESI+0x16]` (`89 46 16`).
- Assumes command 0x0D is embedded as `C7 46 04 0D 00 00 00` — distinguishes from FUN_101d16c0 (cmd 0x09) which has `56 8B F1 57` + `8B 7D 10` + `C7 07` + `89 46 08`.
- Assumes the INT3 at offset 0x54 is the thread-check breakpoint (command 0x0D, local-mode variant).
- *Provenance: RE-F5-cache-fix (2026-08-26), `docs/RE/archive/RE_F5_cache_fix_findings.md`*

---

### 4.21 Target 21: FUN_101d18c0 thread-check INT3 patch (RVA 0x1D19E6)

| Field | Value |
|---|---|
| **RVA (patch site)** | 0x1D19E6 |
| **VA (patch site)** | 0x101D19E6 |
| **Containing function** | FUN_101d18c0 (RVA 0x1D18C0, VA 0x101D18C0) |
| **Name** | FUN_101d18c0_thread_check_INT3 |
| **Calling convention** | `__thiscall` (ecx=this) |
| **Signature length** | 26 bytes |
| **Patch site?** | Yes |
| **Sig match RVA** | 0x1D19E4 (mid-function anchor — JZ+1 before INT3) |
| **Patch offset** | 0x02 (2) from sig match point |
| **Original byte at patch site** | `CC` (INT3) |
| **Replacement byte** | `90` (NOP) |
| **Exported?** | No (internal function, resolved by RVA) |

**Note:** This signature uses a **mid-function anchor** pattern (not a function-entry match). The pattern starts at the `JZ +1` instruction immediately before the INT3, at RVA 0x1D19E4 (offset 0x124 from function entry 0x1D18C0). The INT3 is at byte offset 2 within the pattern (offset 0x126 = 294 from function entry).

**Signature (mid-function anchor at RVA 0x1D19E4):**
```
74 01 CC FF 76 6E C7 46 04 10 00 00 00 FF 15 88 96 1F 10 6A FF FF 76 6A FF D3
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001D19E4 (mid-function anchor). PASS. *(Source: `signatures/v9.50.01.3344.json`, expectedMatchCount=1)*

**Patch details:**
- Single-byte patch: `0xCC` (INT3) → `0x90` (NOP) at sig match + 0x02 (function entry + 0x126).
- This NOP's the thread-check INT3 in the command 0x10 (BST search non-server mode) dispatch helper, allowing it to be called from any thread.
- Context bytes at patch site: `74 01 CC FF 76 6E C7 46 04 10 00 00 00` (JZ +1; INT3; PUSH [ESI+0x6E]; MOV [ESI+4], 0x10).
- The production code verifies the byte is either `0xCC` or `0x90` before patching; skips idempotently if already `0x90`.

**Structural assumptions:**
- Assumes `__thiscall` convention (ecx=this) — the pattern is mid-function, ESI already holds the this pointer.
- Assumes command 0x10 (BST search non-server mode) is embedded as `C7 46 04 10 00 00 00` (MOV [ESI+4], 0x10) — distinguishes from FUN_101cfce0 (cmd 0x0D) and FUN_101d15c0 (cmd 0x0D).
- Assumes `FF 15 88 96 1F 10` is a FIXED (non-wildcarded) IAT call — unusual in this catalog. The IAT slot address is fixed because it's a known stable import.
- Assumes `FF D3` (CALL EBX) calls WaitForSingleObject or similar.
- Assumes the INT3 at offset 0x02 from the pattern start is the thread-check breakpoint (command 0x10).
- *Provenance: RE-F5-name-getter (2026-08-26), `docs/RE/RE_F5_name_getter_patch_analysis.md §5.3`*

---

### 4.22 Target 22: FUN_101a42d0 find item get name (RVA 0x1A42D0)

| Field | Value |
|---|---|
| **RVA** | 0x1A42D0 |
| **VA** | 0x101A42D0 |
| **Name** | FUN_101a42d0_find_item_get_name |
| **Calling convention** | `__fastcall` (ECX=warehouse, EDX=name) |
| **Signature length** | 81 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 83 EC 48 53 8B D9 89 55 F8 56 8B F3 89 5D F4 57 8D 4E 01 8A 06 46 84 C0 75 F9 2B F1 83 CF FF 85 F6 7E 60 6A 00 E8 ?? ?? ?? ?? 8B F0 C7 45 FC 00 00 00 00 83 C4 04 85 F6 74 49 80 3D ?? ?? ?? ?? 01 75 4B 8B 5D 08 8D 45 FC 50 53 FF 75 F8
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001A42D0 (expected RVA). PASS. *(Source: `signatures/v9.50.01.3344.json`, expectedMatchCount=1)*

**Structural assumptions:**
- Assumes `__fastcall` convention (ECX=warehouse, EDX=name) — `8B D9` (MOV EBX,ECX) and `8B F3` (MOV ESI,EDX) capture args.
- Assumes **NO SEH frame** — minimal prologue (`55 8B EC 83 EC 48`).
- Assumes a 0x48 (72) byte stack frame (`83 EC 48`).
- Assumes an inline `strlen` loop: `8D 4E 01` (LEA ECX,[ESI+1]) → `8A 06` (MOV AL,[ESI]) → `46` (INC ESI) → `84 C0` (TEST AL,AL) → `75 F9` (JNZ -7).
- Assumes `SUB ESI, ECX` (`2B F1`) computes the string length.
- Assumes `OR EDI, -1` (`83 CF FF`) sets EDI to 0xFFFFFFFF (not-found sentinel).
- Assumes `CMP byte ptr [DAT_1023aa8f], 1` (`80 3D ?? ?? ?? ?? 01`) checks a server-mode flag (wildcarded address).
- This is a combined BST search + lazy name fetch function. It was superseded by direct calls to FUN_101d05e0 (BST search) + FUN_101c36d0 (verbatim name getter) but is retained in the catalog.
- Distinctive feature: `83 EC 48` (72-byte frame) + `8B D9` (MOV EBX,ECX) + inline strlen loop (`8A 06 46 84 C0 75 F9`) + `80 3D ?? ?? ?? ?? 01` (server-mode flag check).
- *Provenance: RE-F5-name-getter (2026-08-26), `docs/RE/RE_F5_name_getter_patch_analysis.md`*

---

### 4.23 Target 23: FUN_101d05e0 BST search wrapper (RVA 0x1D05E0)

| Field | Value |
|---|---|
| **RVA** | 0x1D05E0 |
| **VA** | 0x101D05E0 |
| **Name** | FUN_101d05e0_bst_search_wrapper |
| **Calling convention** | `__thiscall` (ecx=warehouse; stack: typeFilter, name, *outNode); RET 0xC |
| **Signature length** | 74 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 83 B9 C2 00 00 00 01 56 75 23 FF 75 0C FF 75 08 E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 8B 75 10 89 06 33 C0 39 06 5E 0F 95 C0 5D C2 0C 00 8B 75 10 56 FF 75 0C FF 75 08 E8 A1 12 00 00 33 C0 39 06 5E 0F 95 C0 5D C2 0C
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001D05E0 (expected RVA). PASS. *(Source: `signatures/v9.50.01.3344.json`, expectedMatchCount=1)*

**Structural assumptions:**
- Assumes `__thiscall` convention: ECX = warehouse object; 3 stack args at `[EBP+0x8]` (typeFilter), `[EBP+0xC]` (name), `[EBP+0x10]` (*outNode pointer); `RET 0xC` (`C2 0C 00`).
- Assumes `CMP [ECX+0xC2], 1` (`83 B9 C2 00 00 00 01`) checks a server-mode flag — if set, takes the server path (JZ to non-server path at `75 23`).
- Assumes in server mode: two relative calls (`E8 ?? ?? ?? ??`) to server-mode dispatch functions, then stores the BST node pointer in `*outNode` (`89 06` = MOV [ESI], EAX where ESI = [EBP+0x10]).
- Assumes in non-server mode: `E8 A1 12 00 00` is a FIXED (non-wildcarded) relative call to FUN_101d18c0 (the non-server dispatch helper). This is the same function patched in Target 21.
- Assumes `SETZ AL` (`0F 95 C0`) sets the return value based on whether the node was found.
- Distinctive feature: `83 B9 C2 00 00 00 01` (CMP [ECX+0xC2],1 — server-mode flag check) + `E8 A1 12 00 00` (CALL FUN_101d18c0 — non-server dispatch).
- *Provenance: RE-F5-name-getter (2026-08-26), `docs/RE/RE_F5_name_getter_patch_analysis.md`*

---

### 4.24 Target 24: FUN_101c36d0 verbatim name getter (RVA 0x1C36D0)

| Field | Value |
|---|---|
| **RVA** | 0x1C36D0 |
| **VA** | 0x101C36D0 |
| **Name** | FUN_101c36d0_verbatim_name_getter |
| **Calling convention** | `__fastcall` (ecx=node pointer) |
| **Signature length** | 80 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 81 EC 98 00 00 00 57 8B F9 80 7F 1C 00 0F 85 ?? ?? ?? ?? 6A 01 FF B7 18 02 00 00 8D 4D E0 E8 ?? ?? ?? ?? 8B CF C7 45 FC 00 00 00 00 C6 47 1C 01 E8 ?? ?? ?? ?? 83 7F 10
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001C36D0 (expected RVA). PASS. *(Source: `signatures/v9.50.01.3344.json`, expectedMatchCount=1)*

**Structural assumptions:**
- Assumes `__fastcall` convention (ecx=node pointer) — `8B F9` (MOV EDI,ECX) captures the node pointer.
- Assumes the standard 24-byte SEH frame prologue (SEH handler at bytes 13–16 wildcarded — `MOV EAX, FS:[0]` before `PUSH -1`).
- Assumes a 0x98 (152) byte stack frame (`81 EC 98 00 00 00`).
- Assumes `CMP [EDI+0x1C], 0` (`80 7F 1C 00`) checks a lazy-init flag — if zero, the name hasn't been fetched yet.
- Assumes the lazy-init path calls FUN_101cfce0 (cmd 0x0D) via a relative call to fetch the ProgId, then sets the flag: `MOV [EDI+0x1C], 1` (`C6 47 1C 01`).
- Assumes the verbatim name is stored at node offset +0x1D (returned as char*).
- Assumes `PUSH [EDI+0x218]` (`FF B7 18 02 00 00`) passes a field to the fetch function.
- Distinctive feature: `81 EC 98 00 00 00` (152-byte frame) + `8B F9` (MOV EDI,ECX) + `80 7F 1C 00` (CMP [EDI+0x1C],0 — lazy-init flag).
- *Provenance: RE-F5-name-getter (2026-08-26), `docs/RE/RE_F5_name_getter_patch_analysis.md`*

---

### 4.25 Target 25: FUN_101c36d0 thread-check INT3 patch (RVA 0x1C373A)

| Field | Value |
|---|---|
| **RVA (patch site)** | 0x1C373A |
| **VA (patch site)** | 0x101C373A |
| **Containing function** | FUN_101c36d0 (RVA 0x1C36D0, VA 0x101C36D0) |
| **Name** | FUN_101c36d0_thread_check_INT3 |
| **Calling convention** | `__fastcall` (ecx=node pointer) |
| **Signature length** | 14 bytes |
| **Patch site?** | Yes |
| **Sig match RVA** | 0x1C3736 (mid-function anchor — TEST EAX,EAX before INT3) |
| **Patch offset** | 0x04 (4) from sig match point |
| **Original byte at patch site** | `CC` (INT3) |
| **Replacement byte** | `90` (NOP) |
| **Exported?** | No (internal function, resolved by RVA) |

**Note:** This signature uses a **mid-function anchor** pattern (not a function-entry match). The pattern starts at `TEST EAX,EAX` immediately before the INT3, at RVA 0x1C3736 (offset 0x66 from function entry 0x1C36D0). The INT3 is at byte offset 4 within the pattern (offset 0x6A = 106 from function entry). This patches the **same containing function** as Target 24 (FUN_101c36d0_verbatim_name_getter), but at a different location within it.

**Signature (mid-function anchor at RVA 0x1C3736):**
```
85 C0 75 01 CC 8B 4F 0C 8B 47 10 6A 34 89
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001C3736 (mid-function anchor). PASS. *(Source: `signatures/v9.50.01.3344.json`, expectedMatchCount=1)*

**Patch details:**
- Single-byte patch: `0xCC` (INT3) → `0x90` (NOP) at sig match + 0x04 (function entry + 0x6A).
- This NOP's the thread-check INT3 in the verbatim name getter (FUN_101c36d0). The INT3 fires when FUN_101cfce0 returns 0 (failed to get ProgId).
- Context bytes at patch site: `85 C0 75 01 CC 8B 4F 0C 8B 47 10 6A 34` (TEST EAX,EAX; JNZ +1; INT3; MOV ECX,[EDI+0xC]; MOV EAX,[EDI+0x10]; PUSH 0x34).
- The production code verifies the byte is either `0xCC` or `0x90` before patching; skips idempotently if already `0x90`.

**Structural assumptions:**
- Assumes `__fastcall` convention (ecx=node pointer) — the pattern is mid-function, EDI already holds the node pointer.
- Assumes `TEST EAX,EAX` (`85 C0`) checks the return value of FUN_101cfce0 (cmd 0x0D call).
- Assumes `JNZ +1` (`75 01`) skips the INT3 if the call succeeded (EAX != 0).
- Assumes `PUSH 0x34` (`6A 34`) dispatches command 0x34 after the thread check.
- Assumes the INT3 at offset 0x04 from the pattern start is the thread-check breakpoint (fires on ProgId fetch failure).
- Distinctive feature: `85 C0 75 01 CC` (TEST EAX,EAX; JNZ +1; INT3) + `8B 4F 0C 8B 47 10 6A 34` (MOV ECX,[EDI+0xC]; MOV EAX,[EDI+0x10]; PUSH 0x34 — cmd 0x34 dispatch).
- *Provenance: RE-F5-name-getter (2026-08-26), `docs/RE/RE_F5_name_getter_patch_analysis.md`*

---

## 5. Export Name Confirmation

*Current source: `signatures/v9.50.01.3344.json` (10 by-name exports), `docs/RE/RE_M3_0x40A_NOTIFICATION.md` §1.2 (ordinal 57), and current source resolver names. Historical export evidence (transient/unavailable in the tracked checkout): `tmp/re_artifacts/b1_export_confirmation.md` and `tmp/m1_ghidra_findings.md` §1 (ordinals 66/67).*

> **[HISTORICAL M.1 SNAPSHOT — 2026-08-24 (pre-M.3):** Workstream M.1 added two **ordinal-only
> `[NONAME]` exports** (66 and 67) to the production resolution set,
> replacing the retired `CELCompiler_ctor` / `CELCompiler_dtor` RVA
> signatures (§4.3, §4.4). The post-M.1 phase had 12 tracked exports
> (10 by-name + 2 ordinal-only) — see the Historical snapshots subsection
> above. The two new entries are resolved by ordinal via
> `GetProcAddress(m_orlibModule, MAKEINTRESOURCEA(ordinal))` in
> production and by PE export-table indexing in test mode (see
> `src/orlib_resolver.cpp` `resolveExportByOrdinal()`). They are
> `[NONAME]` exports — they have **no export name string** and cannot be
> resolved by `GetProcAddress(name)`. See the new rows 11–12 in the table
> below and the §5.1 / §5.2 detail subsections. Evidence (historical/transient raw note unavailable in the tracked checkout):
> `tmp/m1_ghidra_findings.md` §1 (R-M.1.1); current handoff:
> `docs/M1_HANDOFF.md`.**]

The 13 tracked export entries used in production code are confirmed present in the tracked baseline fixture `tests/fixtures/orlib20_baseline.dll`'s export table via the historical `dumpbin /exports` record (by-name entries) and historical Ghidra export-table annotations + xrefs (ordinal-only entries). The PE export table has 699 functions total, 636 named, and 63 ordinal-only (`[NONAME]`); that total is distinct from the 13 tracked entries. Ordinal base = 10. The original 7 by-name exports (TVS-B.1) are used in `src/tsdev_lib.cpp`. The 3 by-name exports added in F.4/F.5 are used in the `get-source` and `exists --case-insensitive` code paths. The 3 ordinal-only exports added in M.1/M.3 are used in the CELCompiler object-lifecycle and 0x40A notification paths, replacing the retired §4.3/§4.4 RVA signatures where applicable.

| # | Export name (as used in code) | Decorated? | Ordinal | Hint | Export RVA | Found in export table? | Added in |
|---|---|---|---|---|---|---|---|
| 1 | `_OrlibInit@8` | Yes (stdcall-decorated) | 594 | 24F | 0x0011B4F0 | Yes | TVS-B.1 |
| 2 | `_OrlibExit@0` | Yes (stdcall-decorated) | 593 | 24E | 0x0011C080 | Yes | TVS-B.1 |
| 3 | `FindExistPos` | **No — undecorated** | 453 | 12A | 0x001A4230 | Yes | TVS-B.1 |
| 4 | `_CreateEntryEX@24` | Yes (stdcall-decorated) | 389 | 200 | 0x00159610 | Yes | TVS-B.1 |
| 5 | `_SaveModule_New@20` | Yes (stdcall-decorated) | 634 | 25D | 0x00159A70 | Yes | TVS-B.1 |
| 6 | `_VerifyModule_New@20` | Yes (stdcall-decorated) | 700 | 274 | 0x00169630 | Yes | TVS-B.1 |
| 7 | `_CloseWarehouseForCurrentThreadAndPath@4` | Yes (stdcall-decorated) | 377 | 1F9 | 0x001A9940 | Yes | TVS-B.1 |
| 8 | `_LoadTextFileEX@16` | Yes (stdcall-decorated) | — | — | 0x001081A0 | Yes | F.4 |
| 9 | `_LoadMasterRecsEX@16` | Yes (stdcall-decorated) | — | — | 0x001A5360 | Yes | F.5 |
| 10 | `_GetMasterRecByType@16` | Yes (stdcall-decorated) | — | — | 0x001A0460 | Yes | F.5 |
| 11 | ordinal 66 (`[NONAME]`) | **N/A — ordinal-only, no name** | 66 | — | 0x00165F70 | Yes (ordinal-only `[NONAME]`) | M.1 |
| 12 | ordinal 67 (`[NONAME]`) | **N/A — ordinal-only, no name** | 67 | — | 0x00165FD0 | Yes (ordinal-only `[NONAME]`) | M.1 |
| 13 | ordinal 57 (`[NONAME]`) | **N/A — ordinal-only, no name** | 57 | — | 0x001526F0 | Yes (ordinal-only `[NONAME]`) | M.3 |

**All 13 tracked export entries are present in the export table.** Exports 1–10 are exported by name (none of those 10 are ordinal-only). Exports 11–13 are **ordinal-only `[NONAME]`** — they have no export name string and are resolved by ordinal via `GetProcAddress` with `MAKEINTRESOURCEA(ordinal)` (production) or PE export-table indexing (test mode). Ordinal/hint values for by-name exports 8–10 are not documented in the
historical `b1_export_confirmation.md` (which predates F.4/F.5); the RVAs are
from `signatures/v9.50.01.3344.json`. The RVAs and ordinals for entries 11–12
were established by the historical/transient `tmp/m1_ghidra_findings.md` §1
(unavailable in the tracked checkout); ordinal 57 is from the tracked
`docs/RE/RE_M3_0x40A_NOTIFICATION.md` §1.2 (R-M.3.1). The 13 tracked entries are not the total PE export count.

### 5.1 Ordinal 66 — CELCompiler alloc+ctor (`[NONAME]` export, added in M.1)

*Historical/transient source (unavailable in the tracked checkout): `tmp/m1_ghidra_findings.md` §1 (R-M.1.1)*

| Field | Value |
|---|---|
| **Ordinal** | 66 |
| **Export RVA** | 0x00165F70 |
| **VA** | 0x10165F70 |
| **Ghidra symbol** | FUN_10165f70 |
| **Exported by name?** | **No** — ordinal-only `[NONAME]` export; no name string in the export table. Resolved by `GetProcAddress(m_orlibModule, MAKEINTRESOURCEA(66))` (production) or PE export-table indexing by ordinal (test mode). |
| **Calling convention** | `__cdecl`, no args |
| **Signature** | `void* f()` — takes no arguments, returns a pointer to the newly allocated and constructed CELCompiler object. |
| **Semantics** | `operator_new(0x58)` followed by the CELCompiler constructor `FUN_10165a00` (RVA 0x165A00 — the function previously targeted by the now-retired §4.3 `CELCompiler_ctor` signature). Allocates the 0x58-byte CELCompiler object and constructs it in one call. |
| **Replaces** | §4.3 `CELCompiler_ctor` (RVA 0x165A00) RVA-signature resolution. M.1 retired that signature; the constructor is now reached indirectly via this ordinal 66 thunk. |
| **Caller responsibility** | Caller must eventually free the returned object via **ordinal 67** (which performs dtor + `operator delete(p, 0x58)`). Do **not** call `free`/`operator delete` directly. |

### 5.2 Ordinal 67 — CELCompiler dtor+free (`[NONAME]` export, added in M.1)

*Historical/transient source (unavailable in the tracked checkout): `tmp/m1_ghidra_findings.md` §1 (R-M.1.1)*

| Field | Value |
|---|---|
| **Ordinal** | 67 |
| **Export RVA** | 0x00165FD0 |
| **VA** | 0x10165FD0 |
| **Ghidra symbol** | FUN_10165fd0 |
| **Exported by name?** | **No** — ordinal-only `[NONAME]` export; no name string in the export table. Resolved by `GetProcAddress(m_orlibModule, MAKEINTRESOURCEA(67))` (production) or PE export-table indexing by ordinal (test mode). |
| **Calling convention** | `__stdcall`, one stack arg |
| **Signature** | `void f(void* p)` — takes a pointer to the CELCompiler object (as returned by ordinal 66), returns nothing. |
| **Semantics** | Calls the CELCompiler destructor `FUN_10165ac0` (RVA 0x165AC0 — the function previously targeted by the now-retired §4.4 `CELCompiler_dtor` signature) on `p`, then calls `operator delete(p, 0x58)` to free the 0x58-byte allocation. **Ordinal 67 frees the object internally** — callers must not double-free. |
| **Replaces** | §4.4 `CELCompiler_dtor` (RVA 0x165AC0) RVA-signature resolution. M.1 retired that signature; the destructor is now reached indirectly via this ordinal 67 thunk. |
| **Caller responsibility** | Pass the pointer obtained from ordinal 66. Do not call any other free on the same pointer. |

### 5.3 Ordinal 57 — 0x40A library-change notification sender (`[NONAME]` export, added in M.3)

*Source: `docs/RE/RE_M3_0x40A_NOTIFICATION.md` §1.2 (R-M.3.1)*

| Field | Value |
|---|---|
| **Ordinal** | 57 |
| **Export RVA** | 0x001526F0 |
| **VA** | 0x101526F0 |
| **Ghidra symbol** | FUN_101526f0 |
| **Exported by name?** | **No** — ordinal-only `[NONAME]` export; no name string in the export table. Resolved by `GetProcAddress(m_orlibModule, MAKEINTRESOURCEA(57))` (production) or PE export-table indexing by ordinal (test mode). |
| **Calling convention** | `__stdcall`, two stack args, `RET 8` (C2 08 00) |
| **Signature** | `void f(unsigned short category, unsigned short progId)` — takes the affected module's category (whType low 8 bits) and ProgId (warehouse record index), returns nothing (the disassembly unconditionally `MOV EAX,1` before `RET 8`, but the return value is NOT a delivery guarantee — see §3.1 of the RE note). |
| **Semantics** | Packs `lParam=(category<<16)\|(progId&0xFFFF)`, `wParam=0`, `msg=0x40A`, loads the `tslib::EventsViaHandles` singleton (`PTR_DAT_10231050`) into `ECX` as `this`, and calls `tslib!PosMessageToClients` — the **asynchronous** `PostMessageW`-based variant that iterates the registered client HWND set and posts `0x40A` once per registered handle. Frida-confirmed: 57 total `PostMessageW(0x40A)` sends to 19 distinct receiver HWNDs after a single verify-success operation (3 operations × 19 HWNDs). |
| **Packing (R-M.3.1)** | `msg=0x040A`, `wParam=0`, `lParam=(category<<16)\|(ProgId&0xFFFF)`. The **low word of `lParam` IS the ProgId** (warehouse record index), NOT the subtype. Confirmed by three converging evidence lines (Frida trace identity, Ghidra static field layout, Frida `lParam=0x1a1e/0x1a1f/0x1a20` = ProgIds 6686/6687/6688). |
| **Error semantics (R-M.3.3)** | Always returns 1 regardless of how many HWNDs received the message or whether any `PostMessageW` succeeded. Send failure is silently ignored; no receiver = silent no-op. Operation success is NOT blocked by receiver absence. Callers must NOT treat the return as a delivery guarantee. |
| **TSDev callers** | `FUN_00453f70` (verify-success handler, xref from `0x00453f89`) and `FUN_0044cab0` (rename / change-analysis-technique-type, xref from `0x0044cd8a`). TSDev does NOT import the synchronous variant (ordinal 65, `FUN_101526c0`, which calls `SendMessageToClients`). |
| **Operation set (R-M.3.2)** | verify-success (insert/modify/upsert) sends 0x40A via this ordinal; delete sends 0x40A via the internal `FUN_10159ae0` (gated `category != 4`); failed compile/verify does NOT send. The CLI's `deleteEntry()` calls the orchestrator `FUN_101a8360` (NOT `FUN_10159dd0`/`FUN_10159ae0`), so it posts 0x40A explicitly via this ordinal for parity, gated `category != 4`. |
| **Caller responsibility** | Pass `getCategory(type)` as `category` and `progId & 0xFFFF` as `progId`. Do not interpret the return value as delivery success. |

### `FindExistPos` decorated-form check

The production code resolves `FindExistPos` by its **undecorated** name (not `_FindExistPos@8`). Both forms were checked:

- `FindExistPos` (undecorated) → **FOUND** (ordinal 453, RVA 0x001A4230).
- `_FindExistPos@8` (stdcall-decorated) → **NOT FOUND.** No line in the export table matches `_FindExistPos@8`.

Note: a *different* function `_FindExistPosSubType@12` exists (ordinal 454, RVA 0x001A41B0), but that is a separate export, not the decorated form of `FindExistPos`. The production code's claim that `FindExistPos` is exported undecorated is **CONFIRMED** — the decorated form does not exist.

### Calling conventions for the 13 exports

*Current source: `signatures/v9.50.01.3344.json` (10 by-name exports) and the current resolver declarations/definitions; `docs/RE/RE_M3_0x40A_NOTIFICATION.md` §1.2 (ordinal 57). Historical export evidence (transient/unavailable in the tracked checkout): `tmp/re_artifacts/b1_rva_inventory.md` (§Calling-convention summary) and `tmp/m1_ghidra_findings.md` §1 (ordinals 66/67).*

| Export | Convention | Signature |
|---|---|---|
| `_OrlibInit@8` | `__stdcall` | (const char* workDirPath, int contextId) → BOOL |
| `_OrlibExit@0` | `__stdcall` | () → BOOL |
| `FindExistPos` | `__stdcall` | (const char* name, int warehouseType) → int |
| `_CreateEntryEX@24` | `__stdcall` | (int whType, int secondary, const char* name, int param4, char param5, int* errCode) → int |
| `_SaveModule_New@20` | `__stdcall` | (int warehouseType, int progId, void* arg3, HWND editorHwnd, void* arg5) → int |
| `_VerifyModule_New@20` | `__stdcall` | (HWND hwnd, int warehouseType, int progId, int flag, void* compilerObj) → int |
| `_CloseWarehouseForCurrentThreadAndPath@4` | `__stdcall` | (int param) → void |
| `_LoadTextFileEX@16` | `__stdcall` | (int whType, int progId, char** outText, int* outSize) → int — source text retrieval (F.4) |
| `_LoadMasterRecsEX@16` | `__stdcall` | (int whType, void* callback, int param3, int param4) → int — master record enumeration (F.5) |
| `_GetMasterRecByType@16` | `__stdcall` | (int whType, int index, void* outRec, int param4) → int — master record by type (F.5) |
| ordinal 66 (`[NONAME]`) | `__cdecl` | () → `void*` — `operator_new(0x58)` + ctor `FUN_10165a00`; CELCompiler alloc+ctor (M.1) |
| ordinal 67 (`[NONAME]`) | `__stdcall` | (`void* p`) → void — dtor `FUN_10165ac0` + `operator delete(p, 0x58)`; CELCompiler dtor+free, frees internally (M.1) |
| ordinal 57 (`[NONAME]`) | `__stdcall` | (`unsigned short category, unsigned short progId`) → void — packs `lParam=(category<<16)\|(progId&0xFFFF)`, `wParam=0`, `msg=0x40A`, calls `tslib!PosMessageToClients`; always returns 1 (NOT a delivery guarantee); 0x40A library-change notification sender (M.3) |

---

## 6. Data-Section RVA Targets

*Current source: `signatures/v9.50.01.3344.json` (`dataGlobals`); historical inventory/Ghidra sources (transient/unavailable in the tracked checkout): `tmp/re_artifacts/b1_rva_inventory.md` (entries 13–15) and `tmp/re_artifacts/b1_ghidra_evidence.md` (§3 Data-Section RVA Targets).*

These 3 data-section RVAs are **NOT code signatures** — they are data globals read directly by the production code. They are documented here for completeness but are NOT part of the 22 code-section signature targets.

| # | RVA | VA | Name | Purpose | Access method |
|---|---|---|---|---|---|
| 13 | 0x23CEA0 | 0x1023CEA0 | DAT_1023cea0 | Global pointer to error-record array | `*(uint8_t**)(base + RVA)` — direct memory read |
| 14 | 0x23CEA4 | 0x1023CEA4 | DAT_1023cea4 | Global uint32 count of error records | `*(uint32_t*)(base + RVA)` — direct memory read |
| 15 | 0x23AAB8 | 0x1023AAB8 | DAT_1023aab8 | HINSTANCE for LoadStringA fallback (string table) | `*(HINSTANCE*)(base + RVA)` — direct memory read |

### Resolution strategy (deferred)

Data-section RVAs cannot be matched by instruction-pattern signatures (there are no instruction patterns in data). For robustness across DLL versions, these should be resolved by:

1. Finding the **referencing code function** by its code signature (e.g., `StartCompilingProcess` at RVA 0x165DE0 reads `DAT_1023cea0` at instruction RVA 0x165DF9 via `MOV ECX, [0x1023cea0]`).
2. Extracting the **data reference** from the disassembled instruction (the 4-byte absolute address operand).
3. Converting the absolute address to an RVA by subtracting the image base.

This approach ties data-offset resolution to code-signature resolution, making both robust to DLL version changes (as long as the referencing function's signature still matches). Implementation of this strategy is **deferred to TVS-B.2/B.3**.

---

## 7. Structural Constants

*Historical source (transient/unavailable in the tracked checkout): `tmp/re_artifacts/b1_rva_inventory.md` (entries 16–29). Current values are represented by the tracked JSON and the production source.*

These constants are used by the production code and/or embedded in signatures. They are documented here as part of the RE baseline.

### Constants embedded in signatures (fixed bytes, NOT wildcarded)

| Constant | Value | Name | Where embedded |
|---|---|---|---|
| Error record stride | 0x129 (297) | kErrRecordStride | Target 1 (StartCompilingProcess): `69` opcode prefix of `IMUL EAX, EAX, 0x129` |
| Stack frame size | 0x194 (404) | — | Target 7 (GetWarehouse): `81 EC 94 01 00 00` (SUB ESP, 0x194) |
| Stack frame size | 0x2DC (732) | — | Target 8 (FUN_101a8360): `81 EC DC 02 00 00` (SUB ESP, 0x2DC) |
| Stack frame size | 0x28 (40) | — | Target 6 (DeleteFromTree): `83 EC 28` (SUB ESP, 0x28) |
| Stack frame size | 0x08 (8) | — | Target 3 (CELCompiler ctor): `83 EC 08` (SUB ESP, 8) |
| Allocation size | 0x2C (44) | — | Target 5 (InitGlobalLookup): `6A 2C` (PUSH 0x2C) |
| Command dispatch | 0x0F | — | Target 10 (FUN_101d0950): `C7 46 04 0F 00 00 00` (MOV [ESI+4], 0x0F) |
| Command dispatch | 0x17 | — | Target 12 (FUN_101d1fa0): `C7 46 04 17 00 00 00` (MOV [ESI+4], 0x17) |
| Command dispatch | 0x11 | — | Target 13 (FUN_101d0e80): `C7 47 04 11 00 00 00` (MOV [EDI+4], 0x11) |
| Command dispatch | 0x08 | — | Target 14 (FUN_101d0c30): `C7 46 04 08 00 00 00` (MOV [ESI+4], 0x08) |
| Command dispatch | 0x09 | — | Target 15 (FUN_101d16c0): `C7 46 04 09 00 00 00` (MOV [ESI+4], 0x09) |
| Command dispatch | 0x07 | — | Target 16 (FUN_101d0d90): `C7 46 04 07 00 00 00` (MOV [ESI+4], 0x07) |
| Command dispatch | 0x0D | — | Target 19 (FUN_101cfce0): `C7 46 04 0D 00 00 00` (MOV [ESI+4], 0x0D); Target 20 (FUN_101d15c0): same |
| Command dispatch | 0x10 | — | Target 21 (FUN_101d18c0): `C7 46 04 10 00 00 00` (MOV [ESI+4], 0x10) |
| Command dispatch | 0x34 | — | Target 25 (FUN_101c36d0): `6A 34` (PUSH 0x34) |
| Stack frame size | 0x48 (72) | — | Target 22 (FUN_101a42d0): `83 EC 48` (SUB ESP, 0x48) |
| Stack frame size | 0x98 (152) | — | Target 24 (FUN_101c36d0): `81 EC 98 00 00 00` (SUB ESP, 0x98) |
| Type array sentinel | 0x200 | — | Target 17 (FUN_101d43a0): `C7 40 04 00 02 00 00` (MOV [EAX+4], 0x200) |
| Type array sentinel | 0xFFFFFFFF | — | Target 17 (FUN_101d43a0): `C7 40 08 FF FF FF FF` (MOV [EAX+8], -1) |
| Window style/flag | 0x2000 | — | Target 10: `B8 00 20 00 00` (MOV EAX, 0x2000) |
| Command check | 0x1E | — | Target 11 (FUN_101cf5b0): `83 F8 1E` (CMP EAX, 0x1E) |
| One-time-init guard | 0x01 | — | Target 7: `A8 01` (TEST AL, 1) + `83 C8 01` (OR EAX, 1) |

### Constants used in production logic (NOT in signatures)

| Constant | Value | Name | Source |
|---|---|---|---|
| CELCompiler object size | 0x58 (88) | kCELCompilerSize | `src/tsdev_lib.cpp:61-66` — documented size; ordinal 66 performs the allocation |
| Error record stride | 0x129 (297) | kErrRecordStride | `src/tsdev_lib.cpp:165` — bytes per error record in global array |
| Error text offset | 0x29 | kErrTextOffset | `src/tsdev_lib.cpp:166` — offset of null-terminated error text within record |
| Error resource ID offset | 0x12 | kErrResIdOffset | `src/tsdev_lib.cpp:167` — offset of `short` string-table resource ID |
| Error char offset | 0x0A | kErrCharOffset | `src/tsdev_lib.cpp:168` — `int` character offset of error start in source |
| CELCompiler +0x10 field offset | 0x10 | — | `src/tsdev_lib.cpp:1667-1674` — set to the current operation's ProgId |
| CELCompiler +0x18 field offset | 0x18 | — | `src/tsdev_lib.cpp:1667-1674` — set to `m_editorHwnd` |
| CELCompiler +0x28 field offset | 0x28 | — | `src/tsdev_lib.cpp:1667-1674` — not written by TSDev or the CLI; ctor does not zero it |
| CELCompiler +0x10 field value | per-operation ProgId | — | `src/tsdev_lib.cpp:1673` |
| CELCompiler +0x28 field value | indeterminate (not written) | — | `src/tsdev_lib.cpp:1669` |
| Warehouse type: Strategy | 0 | — | `src/tsdev_lib.h:50-55` |
| Warehouse type: Indicator | 2 | — | `src/tsdev_lib.h:50-55` |
| Warehouse type: Function | 4 | — | `src/tsdev_lib.h:50-55` |
| Error count sanity bound | 1000 | — | `src/tsdev_lib.cpp:2630-2631` — guards against corrupt count |
| LoadStringA buffer size | 1024 | — | `src/tsdev_lib.cpp:2665-2666` — `char buf[1024]` |
| Error level (error) | 0x7775 | — | Referenced in `AGENTS.md` only; NOT present as a literal in production source. Error-record level field at +0x00 (0x7775 = error). Recorded for completeness. |

---

## 8. Uniqueness Verification Methodology

*Current implementation: `src/sigtool.cpp`, invoked by `tests/run_regression.ps1`; historical standalone probe and scan record (transient/unavailable in the tracked checkout): `archive/sig_scan.cpp` and `tmp/re_artifacts/b1_sig_scan_run.md`.*

### Probe description

The current `sigtool verify` path verifies each of the 22 candidate signatures
against the `.text` section of `tests/fixtures/orlib20_baseline.dll`. The
archived `archive/sig_scan.cpp` probe and its scan record are historical (the
active CMake target is `sigtool`). The active set was 24 before Workstream M.1,
which retired the `CELCompiler_ctor` and `CELCompiler_dtor` signatures — see the
2026-08-24 correction notice and the RETIRED notes on §4.3/§4.4.

**How it works:**

1. **Reads the DLL from disk** (does NOT load it into memory). The entire file is read into a `std::vector<uint8_t>`.
2. **Parses PE headers**: DOS header → `e_lfanew` → NT headers (PE32). Validates DOS signature (`MZ`), PE signature (`PE\0\0`), and optional header magic (`0x010B` = PE32). Asserts `sizeof(void*) == 4` (32-bit build).
3. **Locates the `.text` section** by iterating section headers (`IMAGE_FIRST_SECTION` macro) and matching the name `.text`. Extracts: virtual address (RVA), virtual size, raw file offset, raw size.
4. **Parses each signature** from `signatures/v9.50.01.3344.json` into a parallel byte array + wildcard mask (`Pattern` struct). `??` tokens become wildcards; `XX` tokens are fixed bytes.
5. **Scans the `.text` raw bytes** with wildcard-aware byte matching: for each position `i` in `[0, textRawSize - patLen]`, checks if all non-wildcard bytes match. Collects all match offsets.
6. **Converts match offsets to RVAs**: `RVA = textRVA + offsetInText`. Reports file offset and RVA for each match.
7. **PASS criteria**: exactly 1 match AND the match RVA equals the expected RVA from `signatures/v9.50.01.3344.json`.
8. **Patch-site verification**: for patch sites, reads the byte at `matchOffset + patchOffset` and compares it to the first byte of `originalBytes`. Reports OK if they match, WARNING if they differ (may already be patched).
9. **Exit code**: 0 = all 22 PASS; 1 = at least one FAIL.

### Build and run

```
cmake -S . -B build -A Win32
cmake --build build --config Release --target sigtool
build\Release\sigtool.exe verify --dll tests\fixtures\orlib20_baseline.dll --signatures signatures\v9.50.01.3344.json
build\Release\sigtool.exe verify-patch --dll tests\fixtures\orlib20_baseline.dll --signatures signatures\v9.50.01.3344.json
```

### Result

> **[M.1 UPDATE — 2026-08-24:** The active signature count is now **22**.
> Targets #3 `CELCompiler_ctor` and #4 `CELCompiler_dtor` were retired by
> Workstream M.1 and are no longer part of the active `sigtool` scan or generated into
> `signatures/v9.50.01.3344.json`. Their historical scan rows are retained
> below marked **RETIRED 2026-08-24 (M.1)** for traceability; they are
> excluded from the active 22/22 PASS count. The constructor/destructor
> are now reached via ordinal 66/67 export resolution (see §5.1/§5.2 and
> `docs/M1_HANDOFF.md`).**]

**Historical scan record (unverified current run; not current acceptance evidence): 22 / 22 signatures PASS. Exit code 0.** (Pre-M.1: 24 / 24 signatures PASS — the two retired rows below were part of that historical 24/24 result.)

The historical scan record reports all 22 active signatures matched exactly once at their expected RVAs within the `.text` section and all 11 patch-site byte checks passed (the byte at each patch offset matches the expected original byte). This is not a current acceptance result. The original TVS-B.1 signatures (targets 1–8, 10–12; 11 remaining after TVS-L.3 retired target 9; 9 remaining after M.1 retired targets 3 and 4) were covered by the unavailable historical `b1_sig_scan_run.md` record (which originally verified 12; target 9 was removed in TVS-L.3; targets 3 and 4 were removed in M.1). The 13 new signatures (targets 13–25) were covered by the historical sigtool regression record (`tests/run_regression.ps1`); the current implementation is `src/sigtool.cpp` using `signatures/v9.50.01.3344.json` with the 22 active signatures.

| # | Name | Expected RVA | Match Count | Matched RVA | Result |
|---|---|---|---|---|---|
| 1 | StartCompilingProcess | 0x00165DE0 | 1 | 0x00165DE0 | PASS |
| 2 | EndCompilingProcess | 0x00165ED0 | 1 | 0x00165ED0 | PASS |
| 3 | CELCompiler_ctor | 0x00165A00 | 1 | 0x00165A00 | PASS — **RETIRED 2026-08-24 (M.1)** (historical row; no longer scanned) |
| 4 | CELCompiler_dtor | 0x00165AC0 | 1 | 0x00165AC0 | PASS — **RETIRED 2026-08-24 (M.1)** (historical row; no longer scanned) |
| 5 | InitGlobalLookup | 0x001A9460 | 1 | 0x001A9460 | PASS |
| 6 | DeleteFromTree | 0x001E4030 | 1 | 0x001E4030 | PASS |
| 7 | GetWarehouse | 0x001A9650 | 1 | 0x001A9650 | PASS |
| 8 | FUN_101a8360_delete_orchestrator | 0x001A8360 | 1 | 0x001A8360 | PASS |
| 10 | FUN_101d0950_thread_check_INT3 | 0x001D0950 | 1 | 0x001D0950 | PASS (patch byte 0xCC OK) |
| 11 | FUN_101cf5b0_thread_check_INT3 | 0x001CF5B0 | 1 | 0x001CF5B0 | PASS (patch byte 0xCC OK) |
| 12 | FUN_101d1fa0_thread_check_INT3 | 0x001D1FA0 | 1 | 0x001D1FA0 | PASS (patch byte 0xCC OK) |
| 13 | FUN_101d0e80_thread_check_INT3 | 0x001D0E80 | 1 | 0x001D0E80 | PASS (patch byte 0xCC OK) |
| 14 | FUN_101d0c30_thread_check_INT3 | 0x001D0C30 | 1 | 0x001D0C30 | PASS (patch byte 0xCC OK) |
| 15 | FUN_101d16c0_thread_check_INT3 | 0x001D16C0 | 1 | 0x001D16C0 | PASS (patch byte 0xCC OK) |
| 16 | FUN_101d0d90_thread_check_INT3 | 0x001D0D90 | 1 | 0x001D0D90 | PASS (patch byte 0xCC OK) |
| 17 | FUN_101d43a0_build_type_array | 0x001D43A0 | 1 | 0x001D43A0 | PASS |
| 18 | FUN_1000dd30_celprogwriter_ctor | 0x0000DD30 | 1 | 0x0000DD30 | PASS |
| 19 | FUN_101cfce0_thread_check_INT3 | 0x001CFCE0 | 1 | 0x001CFCE0 | PASS (patch byte 0xCC OK) |
| 20 | FUN_101d15c0_thread_check_INT3 | 0x001D15C0 | 1 | 0x001D15C0 | PASS (patch byte 0xCC OK) |
| 21 | FUN_101d18c0_thread_check_INT3 | 0x001D19E4 | 1 | 0x001D19E4 | PASS (patch byte 0xCC OK) |
| 22 | FUN_101a42d0_find_item_get_name | 0x001A42D0 | 1 | 0x001A42D0 | PASS |
| 23 | FUN_101d05e0_bst_search_wrapper | 0x001D05E0 | 1 | 0x001D05E0 | PASS |
| 24 | FUN_101c36d0_verbatim_name_getter | 0x001C36D0 | 1 | 0x001C36D0 | PASS |
| 25 | FUN_101c36d0_thread_check_INT3 | 0x001C3736 | 1 | 0x001C3736 | PASS (patch byte 0xCC OK) |

*(Source: `b1_sig_scan_run.md` for targets 1–8, 10–12; `tests/run_regression.ps1` sigtool suite for targets 13–25. All 22 active signatures verified unique with expectedMatchCount=1 in `signatures/v9.50.01.3344.json` (post-M.1). The two retired rows (#3, #4) are historical and excluded from the active 22/22 PASS count.)*

**Note on signature #12 evolution:** During the first scan run, signature #12 (FUN_101d1fa0) was 41 bytes and matched twice (a spurious match at RVA 0x001CFCE0 in addition to the expected 0x001D1FA0). The signature was extended to 57 bytes by appending the post-INT3 context bytes (`FF 76 6E C7 46 04 17 00 00 00 FF 15 ?? ?? ?? ??`), which disambiguated the duplicate. The re-run confirmed uniqueness (count = 1, exit 0). The 57-byte version is the finalized signature in `signatures/v9.50.01.3344.json`.

**Note on mid-function anchor signatures (targets 21, 25):** These signatures match at a mid-function anchor point rather than the function entry. The `expectedRva` in the JSON for these entries is the containing function entry, but the actual pattern match occurs at a later offset within the function. The current `sigtool` path verifies uniqueness (match count = 1) for all 22 active signatures; the archived `sig_scan` probe is historical.

---

## 9. Open Items / Deferred

The following items were **out of scope for TVS-B.1** and have since been completed by later TVSs:

| Item | Status | Description |
|---|---|---|
| Data-section RVA resolution | DONE (TVS-B.4/B.4.5) | The 3 data-section RVAs (0x23CEA0, 0x23CEA4, 0x23AAB8) are now resolved via signature-based instruction-operand extraction. See §6. |
| Signature-based RVA resolution integration | DONE (TVS-B.4/B.5) | The 22 signatures in this catalog are now integrated into the production code. Hardcoded RVAs have been replaced with signature-scanned RVAs at runtime via `OrlibResolver`. (Was 24 prior to M.1, which retired the `CELCompiler_ctor`/`CELCompiler_dtor` signatures in favor of ordinal 66/67 export resolution.) |
| Version detection logic | DONE (TVS-B.2/B.4) | The DLL baseline metadata (FileVersion, SHA-256, PE timestamp) documented in §1 is now used by the 3-tier version detection logic implemented in `OrlibResolver::detectVersion()`. |
| SEH protection on data-array reads | DONE (TVS-B.5, partial) | `readErrorsFromArray` now has SEH protection around the array pointer dereference and record iteration. |
| Warehouse type resolution | DONE for TradingApp (TVS-C.1–C.7) and ShowMe (TVS-C.8); 3 types remain unconfirmed | The authoritative warehouse-type→`whType` mapping (5 confirmed, 3 unconfirmed) lives in `docs/TS_COMPONENT_TYPES.md` (that category's SSOT; code SSOT: `src/tsdev_lib.h` enum). |

---

## Appendix A: Acceptance Criteria Traceability

| Criterion (`docs/archive/HANDOFF_B_ROBUSTNESS.md` §4) | Where satisfied |
|---|---|
| 1. Document lists, for each of 22 RVA targets: RVA, function/patch name, candidate signature (hex with wildcard), signature length, uniqueness-verification result (count = 1 in .text), expected original bytes at patch site (for patches), calling convention, structural assumptions | §3 (catalog table + patch metadata table), §4 (per-target detail sections with all fields). (Was 24; targets #3/#4 retired in M.1 — see 2026-08-24 correction notice and RETIRED notes on §4.3/§4.4.) |
| 2. Each signature verified unique by scanning actual orlib20.dll .text section | §8 (methodology + historical 22/22 PASS record; current implementation is `src/sigtool.cpp` and the `archive/sig_scan.cpp` probe is historical) |
| 3. For exported functions (§2.4), catalog notes decorated/undecorated name and confirms it appears in export table | §5 (13 tracked export entries: 10 by-name exports with decorated/undecorated status, ordinals, RVAs, all confirmed FOUND; 3 ordinal-only `[NONAME]` exports 66, 67, and 57 added in M.1/M.3 with Ghidra-confirmed RVAs; total PE exports are separate) |
| 4. Catalog notes orlib20.dll file version, PE timestamp, file size, SHA-256 hash | §1 (DLL Baseline: FileVersion 9.50.01.3344, PE timestamp 0x5C01735E, file size 4,924,400 bytes, SHA-256 318DB24A...) |
