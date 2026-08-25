# M.1 Handoff — Workstream M, Slice M.1 (TSDev Parity: Compiler-Object Lifecycle)

> **Status:** APPROVED (adversarial review, round 3). Implemented and verified.
> **Date:** 2026-08-24
> **Workstream:** M (TSDev parity) — slice M.1
> **Plan reference:** `docs/workstream_tsdev_parity_plan.md`
> **RE evidence:** `tmp/m1_ghidra_findings.md` (§1–§3), `tmp/m1_trace_reanalysis.md`
> **Catalog impact:** `docs/SIGNATURE_CATALOG.md` (see the 2026-08-24 correction notice at the top of that file)

This document is the durable findings/handoff note for Workstream M slice M.1. It records the research answers (R-M.1.1 through R-M.1.5), the implementation summary, test and E2E results, signature/catalog impact, residual limitations, follow-up work for M.2–M.5, and the adversarial review outcome. It is intended to survive context compaction and to be the authoritative reference for downstream M-slices and for any future re-validation of the M.1 changes.

---

## 1. Research answers (R-M.1.1 through R-M.1.5)

### R-M.1.1 — Ordinal 66 / 67 export identification

Ordinal 66 = `FUN_10165f70` (RVA 0x165F70), `__cdecl`, no args, returns `void*`. Semantics: `operator_new(0x58)` + ctor `FUN_10165a00` (the CELCompiler constructor). Ordinal-only `[NONAME]` export — no name string in the export table; resolved by `GetProcAddress(m_orlibModule, MAKEINTRESOURCEA(66))` (production) or PE export-table indexing by ordinal (test mode).

Ordinal 67 = `FUN_10165fd0` (RVA 0x165FD0), `__stdcall`, one arg (`void* p`), returns void. Semantics: dtor `FUN_10165ac0` (the CELCompiler destructor) + `operator delete(p, 0x58)`. Ordinal-only `[NONAME]` export. Ordinal 67 frees the object internally — callers must not double-free.

**Evidence:** Ghidra export table annotations + xrefs; `tmp/m1_ghidra_findings.md` §1. Cross-checked against the retired §4.3 (`CELCompiler_ctor`, RVA 0x165A00) and §4.4 (`CELCompiler_dtor`, RVA 0x165AC0) RVA-signature targets — ordinal 66 reaches the same constructor via the `FUN_10165f70` thunk, and ordinal 67 reaches the same destructor via the `FUN_10165fd0` thunk.

### R-M.1.2 — CELCompiler field writes (category / subtype / ProgId / HWND)

The CELCompiler object fields are written as follows:

- `+0x08` = category
- `+0x0C` = subtype
- `+0x10` = ProgId
- `+0x18` = HWND (notification window)

These fields are written by `FUN_00451800` at instruction range `0x00451ac4`–`0x00451af9` (TSDev.exe). The `+0x00` and `+0x04` fields are zeroed by `FUN_004D9480` after `StartCompilingProcess`, before the verify call.

**Evidence:** `tmp/m1_ghidra_findings.md` §2.

### R-M.1.3 — Field +0x28 is NOT written by TSDev (uninitialized heap)

The CELCompiler constructor (`FUN_10165a00`) does **NOT** zero `+0x28`. The ctor zeroes `+0x04`, `+0x08`, `+0x0C`, `+0x10`, `+0x14`, `+0x18`, and `+0x2C`, but **not** `+0x28`. Consequently `+0x28` is **uninitialized heap** — its runtime value is indeterminate and must not be asserted or relied upon.

This finding retired the pre-M.1 production write that set `+0x28 = 6` after construction. M.1 removed that write. The retired `+0x10 = 0xA02` write was also removed (replaced by `+0x10 = progId` per R-M.1.2).

**Evidence:** `tmp/m1_ghidra_findings.md` §3 (ctor disassembly showing the zeroed-offset set excludes +0x28).

### R-M.1.4 — Field +0x18 = notification HWND

`+0x18` is the notification HWND, obtained from `CELProperties::GetNotificationWindow()`, which returns the active edit view's HWND. A Frida trace confirms that, for one captured UI operation, `_SaveModule_New`'s `editorHwnd` argument equals `_VerifyModule_New`'s `hwnd` argument (pointer identity).

TSDev does **not** import `_SaveModule_New` into the TSDev automation code path, so a direct save-chain comparison is not available statically. The `+0x18` value used by TSDev automation is `m_editorHwnd` (the same notification HWND), consistent with the captured Frida trace.

**Evidence:** `tmp/m1_ghidra_findings.md` §2; `tmp/m1_trace_reanalysis.md`.

### R-M.1.5 — Category/subtype source (warehouse type mapping, NOT source-text parsing)

Category and subtype are obtained from the warehouse type mapping (`getCategory` / `getSubType`), **not** by parsing EasyLanguage source text. TSDev reads category from `CDocument+0x1e8` and subtype from `CDocument+0x1ea`. This is consistent with the project's hard rule that metadata must come from orlib20 warehouse records/APIs, never from item source content.

**Evidence:** `tmp/m1_ghidra_findings.md` §2.

---

## 2. Implementation summary

### Compiler-object lifecycle

Replaced the pre-M.1 `std::malloc` + `memset` + ctor + dtor + `free` sequence with the ordinal-based lifecycle:

- **Allocation + construction:** ordinal 66 (`FUN_10165f70`, `__cdecl`, `void* f()`) — performs `operator_new(0x58)` + ctor `FUN_10165a00` in one call. Returns the constructed CELCompiler pointer.
- **Destruction + free:** ordinal 67 (`FUN_10165fd0`, `__stdcall`, `void f(void* p)`) — performs dtor `FUN_10165ac0` + `operator delete(p, 0x58)`. Frees internally; no double-free.

### Resolver

Added `resolveExportByOrdinal()` to `src/orlib_resolver.cpp` / `src/orlib_resolver.h` with two paths:

- **Production path:** `GetProcAddress(m_orlibModule, MAKEINTRESOURCEA(ordinal))` — the canonical way to resolve an ordinal-only `[NONAME]` export.
- **Test-mode path:** PE export-table indexing by ordinal (parses the export directory, walks the ordinal-indexed array). Used by unit tests that load the baseline DLL fixture without going through the live `GetProcAddress` path.

### Field writes (per R-M.1.2 / R-M.1.5)

- `+0x08` = `getCategory(whType)`
- `+0x0C` = `getSubType(whType)`
- `+0x10` = `progId`
- `+0x18` = `m_editorHwnd`

### Removed writes

- Removed the pre-M.1 `+0x10 = 0xA02` write (replaced by `+0x10 = progId`).
- Removed the pre-M.1 `+0x28 = 6` write (per R-M.1.3 — `+0x28` is uninitialized heap, not written by TSDev).

### Zeroing

Zero `+0x00` and `+0x04` after `StartCompilingProcess`, before `_VerifyModule_New` (per R-M.1.2 — these fields are zeroed by `FUN_004D9480` in TSDev at that point in the flow).

### Test-only hook

`onCompilerReady` hook is **test-only** (`nullptr` in production). It allows unit tests to inspect/modify the compiler object between construction and verify; production never sets it.

### No double-free

Ordinal 67 frees the object internally (`dtor` + `operator delete(p, 0x58)`). The production code calls ordinal 67 exactly once per object and does not call any other free on the same pointer.

### Files changed

- `src/tsdev_lib.cpp`
- `src/tsdev_lib.h`
- `src/orlib_resolver.cpp`
- `src/orlib_resolver.h`
- `src/tsdev_lib_test_hooks.h`
- `src/create_path_cleanup.h`
- `signatures/v9.50.01.3344.json`
- `tools/sigcodegen.py`
- `tests/test_compiler_layout.cpp`
- `tests/test_orlib_resolver.cpp`
- `tests/test_sigtool.ps1`
- `CMakeLists.txt`
- `tests/run_unit_tests.ps1`

For larger changes, see the repository paths above and their current line numbers; code excerpts are intentionally kept short in this handoff. The authoritative per-target detail for the retired ctor/dtor signatures and the new ordinal 66/67 exports is in `docs/SIGNATURE_CATALOG.md` (§4.3, §4.4, §5.1, §5.2).

---

## 3. Test and E2E results

| Suite | Result |
|---|---|
| `test_orlib_resolver` | 251/251 × 3 cycles, 0 failures |
| `test_no_hardcoded_rvas` | 25 forbidden entries, 0 violations |
| `test_sigtool` | 12/12 passed |
| `test_compiler_layout` | 18/18 assertions, 0 failures (RED: 2 FAILs + SEH crash with old field writes; GREEN: 18/18 with new field writes) |
| Full regression | 12/12 suites PASS |

Full regression coverage:

- 253 unknown-link assertions
- 34 E2E CLI cases
- all 5 component types (Strategy, Indicator, Function, TradingApp, ShowMe)
- batch / stdin / manifest coverage
- serve E2E **skipped** (elevation requirement — see §4 Residual limitations)
- stress suite **opt-in, skipped**

The `test_compiler_layout` RED→GREEN transition is the key TDD evidence: with the pre-M.1 field writes (`+0x10 = 0xA02`, `+0x28 = 6`), the suite produced 2 FAILs and an SEH crash; with the M.1 field writes (`+0x10 = progId`, no `+0x28` write, `+0x08`/`+0x0C`/`+0x18` per R-M.1.2/R-M.1.5), the suite passes 18/18.

---

## 4. Residual limitations

- **`+0x28` runtime value is indeterminate** (uninitialized heap, per R-M.1.3). It is not asserted in tests. Verified by code review that no write to `+0x28` occurs in the M.1 production path. Any future code that depends on `+0x28` must first establish a TSDev-observed write to that offset.
- **Serve E2E not run** (elevation requirement). Serve dispatches to the same `verifyModule` function exercised by the other suites, so the M.1 changes are covered by those suites. Serve E2E remains pending an elevated run.
- **No live compiler-object byte dump was performed.** Static Ghidra analysis plus existing Frida traces were cross-checked to confirm call arguments and pointer identity (e.g., `_SaveModule_New` `editorHwnd` == `_VerifyModule_New` `hwnd` for one captured UI operation — R-M.1.4). A live byte dump is a possible future hardening step but is not required for M.1 closure.

---

## 5. Signature / catalog impact

| Metric | Before M.1 | After M.1 |
|---|---|---|
| Signature count | 24 | 22 (removed `CELCompiler_ctor`, `CELCompiler_dtor`) |
| Export count | 10 | 12 (added ordinal 66, ordinal 67) |
| Generated header `kSignatureCount` | 24 | 22 |
| Generated header `kExportCount` | 10 | 12 |
| `tools/sigcodegen.py` `EXPECTED_SIGNATURE_COUNT` | 24 | 22 |
| `tools/sigcodegen.py` `EXPECTED_EXPORT_COUNT` | 10 | 12 |

The two retired signatures are retained in `docs/SIGNATURE_CATALOG.md` §4.3/§4.4 with **RETIRED 2026-08-24 (M.1)** notes prepended (per the project rule that historical documentation is corrected with dated notices, not silently rewritten). The two new exports are documented in §5 (table rows 11–12) and §5.1/§5.2 (detail subsections). The catalog's top-of-file 2026-08-24 correction notice is the authoritative supersession statement for all prior 24/10 count references.

---

## 6. Follow-up work for M.2 / M.3 / M.4 / M.5

- **M.2:** Save flags, `CELProperties`, internal encryption save-side parity.
- **M.3:** Post-success `0x40A` library-change notification.
- **M.4:** Dependent revalidation and contract-change handling.
- **M.5:** Consolidated verification and documentation closure.

M.1 established the compiler-object lifecycle foundation (ordinal 66/67 resolution, field-write contract per R-M.1.2/R-M.1.3/R-M.1.5) that M.2–M.4 build on. The residual limitations in §4 should be revisited as appropriate in those slices (e.g., serve E2E elevation, live byte dump).

---

## 7. Adversarial review

3 review rounds; **APPROVED on round 3**.

- **Round 1:** 2 findings.
  - MUST FIX: stale "ctor-zeroed 0" comments about `+0x28` (the ctor does **not** zero `+0x28` per R-M.1.3).
  - SHOULD FIX: stale `std::malloc` reference in `src/create_path_cleanup.h` (M.1 replaced `malloc`+`memset`+ctor with ordinal 66).
- **Round 2:** 1 finding.
  - Same stale "ctor-zeroed" claim about `+0x28` in a test-header comment.
- **Round 3:** APPROVE.
  - All `+0x28` comments consistent and accurate (no claim that the ctor zeroes `+0x28`).
  - The `+0x14` ctor-zeroed claim verified correct (the ctor **does** zero `+0x14`, per R-M.1.3's zeroed-offset set).

---

## 8. Evidence index

| Artifact | Section | Content |
|---|---|---|
| `tmp/m1_ghidra_findings.md` | §1 | R-M.1.1 — ordinal 66/67 export identification (Ghidra export table + xrefs) |
| `tmp/m1_ghidra_findings.md` | §2 | R-M.1.2 / R-M.1.4 / R-M.1.5 — field writes, HWND, category/subtype source |
| `tmp/m1_ghidra_findings.md` | §3 | R-M.1.3 — ctor zeroed-offset set excludes `+0x28` |
| `tmp/m1_trace_reanalysis.md` | — | R-M.1.4 — Frida trace: `_SaveModule_New` `editorHwnd` == `_VerifyModule_New` `hwnd` |
| `docs/SIGNATURE_CATALOG.md` | top notice, §4.3, §4.4, §5, §5.1, §5.2, §8 | Catalog impact: 24→22 signatures, 10→12 exports, retired ctor/dtor, new ordinal 66/67 entries |
| `docs/workstream_tsdev_parity_plan.md` | — | Workstream M plan (M.1–M.5) |
