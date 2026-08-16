# Handoff Document — Priority (C): Support the TradingApp Component Type

**Workstream ID:** C
**Date:** 2026-08-16
**Status:** PLANNED — not yet started
**Parent documents:** `AGENTS.md`, `docs/FEASIBILITY.md`, `docs/TS_COMPONENT_TYPES.md`, `tmp/re_artifacts/orlib_param_orders.md`

---

## 1. Priority Statement

TradeStation's EasyLanguage supports 8 component types, but the tsdev_automation CLI currently supports only 3 (Strategy, Indicator, Function). **TradingApp** — the component type for creating trading applications/gadgets in TradeStation — is a required supported type per the user. It is currently unsupported because its orlib20 warehouse-type code (WHType) is unknown. This workstream discovers that code, adds TradingApp to the CLI's `ComponentType` enum, and verifies that INSERT, DELETE, and MODIFY all work reliably for TradingApp components via the CLI.

---

## 2. Current State

### 2.1 Confirmed warehouse types (3)

| WHType | Component type | Evidence |
|--------|---------------|----------|
| 0 | Strategy | `CreateEntryEX(0,...)` creates entries visible in TSDev strategy list; `FindExistPos(name, 0)` returns ProgId |
| 2 | Indicator | Type converter has 7 subtypes for type 2; confirmed via live RE |
| 4 | Function | `FindExistPos("Average", 4) = 76`; `FindExistPos("MACD", 4) = 20` |

Source: `tmp/re_artifacts/orlib_param_orders.md` §1, `docs/FEASIBILITY.md` §5.2.

### 2.2 Unconfirmed warehouse types (5)

| Component type | Status |
|---------------|--------|
| ActivityBar | WHType NOT confirmed |
| PaintBar | WHType NOT confirmed (may be Indicator subtype 2/3 per §5.2 static survey — UNCONFIRMED) |
| ProbabilityMap | WHType NOT confirmed (may be Indicator subtype 0x12/0x13 per §5.2 — UNCONFIRMED) |
| ShowMe | WHType NOT confirmed (may be Indicator subtype 4/5 per §5.2 — UNCONFIRMED) |
| **TradingApp** | **WHType NOT confirmed — THIS IS THE PRIORITY** |

### 2.3 Existing .ell filename patterns

Files in `MyWork\Bin` follow the pattern `elmodule_<whType>_<progId>.ell`. Observed `<whType>` values in filenames: `{0, 1, 2, 3, 4, 1026, 1282, 4610, 5122}`. The values >255 are composite IDs (subtype<<8 | whType) — all resolve to whType 2 (Indicator subtypes: 1026=0x402, 1282=0x502, 4610=0x1202, 5122=0x1402). The base whType values observed are 0, 1, 2, 3, 4. TradingApp could be whType 1 or 3 (both unconfirmed) or a value not yet observed in filenames.

### 2.4 Current CLI and library state

- **`ComponentType` enum** (`src/tsdev_lib.h` L25-29): `Strategy=0`, `Indicator=2`, `Function=4`. No other types.
- **CLI type parsing** (`src/tsdev_cli.cpp` L57-62): accepts `"strategy"`, `"function"`, `"indicator"` (case-insensitive). No other type strings.
- **CLI subcommands** (`src/tsdev_cli.cpp` L327-354): `insert` and `delete` only. `modify` exists at the library level (`TsDevLib::modifyEntry`) and is tested (`test_modify.cpp`) but is NOT wired into the CLI's `main()` dispatch.
- **All orlib20 primitives are type-agnostic**: `FindExistPos`, `CreateEntryEX`, `SaveModule_New`, `VerifyModule_New`, `GetCompilingErrors`, the delete orchestrator (`FUN_101a8360`), and `DeleteFromTree` all take `whType` as an integer parameter. No type-specific code paths exist in the wrapper. Once the correct whType integer is known, the existing INSERT/DELETE/MODIFY flows should work without code changes beyond adding the enum value.
- **Persistence patch** (`FUN_10011230` at RVA `0x11230`) and **thread-check patches** (RVA `0x1D0993`, `0x1CF5D4`, `0x1D1FC8`) are applied in the `TsDevLib` constructor and are type-agnostic.

---

## 3. Goal

1. **Confirm** the TradingApp warehouse-type code (a single integer) via at least two independent research methods.
2. **Add** `TradingApp = <confirmed_whType>` to the `ComponentType` enum in `src/tsdev_lib.h`.
3. **Add** `"tradingapp"` to the CLI type parser in `src/tsdev_cli.cpp`.
4. **Verify** that INSERT, DELETE, and MODIFY all work reliably for TradingApp components via integration tests (TDD red-green), each independently verified by at least 2 test runs.
5. **Update** documentation (`docs/FEASIBILITY.md` §5.2, `docs/TS_COMPONENT_TYPES.md`, `AGENTS.md` TODO) to reflect the confirmed TradingApp whType.

---

## 4. TVS Decomposition

### TVS-C.1 — Discover the TradingApp warehouse-type code

| Field | Value |
|-------|-------|
| **Type** | RESEARCH |
| **Effort** | M |
| **Goal** | Identify the single integer WHType code that orlib20 uses for TradingApp components. |
| **Scope — DONE** | A confirmed TradingApp WHType integer, cross-validated by at least 2 independent methods. A written findings document in `tmp/re_artifacts/tradingapp_whtype.md`. |
| **Scope — NOT DONE** | No code changes. No CLI changes. No enum changes. No compile/verify testing (that is TVS-C.2). Do NOT investigate other unconfirmed types (ActivityBar, PaintBar, ProbabilityMap, ShowMe). |
| **Acceptance criteria** | (1) A single integer WHType is identified for TradingApp. (2) At least 2 independent methods corroborate the same value. (3) Findings document exists at `tmp/re_artifacts/tradingapp_whtype.md` with evidence (Frida trace logs, disassembly snippets, file listings, or registry screenshots). (4) The document explicitly states which methods were used and how they agree. |
| **Dependencies** | None (first TVS). |
| **Adversarial review** | YES — the whType is the foundation for all subsequent TVSs; an incorrect value would cause every downstream TVS to fail silently or produce wrong-type components. |

**Research methods to try (investigate, do not assume — use whichever work):**

1. **.ell file scan + XML metadata correlation**: List all `elmodule_*.ell` files in `MyWork\Bin`. For each base whType value (0, 1, 2, 3, 4), check the paired `.xml` metadata (root `<symbols>` element, any type/category attribute) or the `MWRKArea.wrk` master records to determine which component type corresponds to that whType. If TradingApp components exist in the library, their `.ell` filenames reveal the whType directly.

2. **Frida trace of TSDev.exe**: Launch TSDev.exe under Frida. Hook `CreateEntryEX` and `FindExistPos` (and optionally `GetMasterRecByName`). Create a new TradingApp component in the TSDev UI. Observe the whType argument passed. This is the most authoritative method — it captures the value the authoritative caller (TSDev.exe) actually uses.

3. **Static analysis of TSDev.exe**: Search TSDev.exe for strings referencing "TradingApp", "Trading App", or related. Trace cross-references to find the code that maps the UI selection to a whType integer passed to `CreateEntryEX`/`FindExistPos`. Use Ghidra MCP tools for decompilation and xref analysis.

4. **Static analysis of orlib20.dll**: Search for "TradingApp" strings or type-name-to-whType dispatch tables. The type converter at RVA `0x1D43A0` (see `docs/TS_COMPONENT_TYPES.md` L43-48) maps whType → subtype lists; check if there is a reverse mapping or a type-name table. Also check the `SigNumToFileName` base-path table at `0x10243000` — the letter encoding (S/F/I/B/M for 0/1/2/3/4) may hint at which whType corresponds to TradingApp.

5. **Registry inspection**: Check `HKLM\SOFTWARE\WOW6432Node\TradeStation` and related keys for TradingApp type registration that might include a numeric type code.

6. **MWRKArea.wrk master record scan**: Use `GetMasterRecByName` or direct warehouse inspection to enumerate all master records, looking for known TradingApp component names (if any exist in the library). The whType parameter that successfully finds them is the TradingApp whType.

---

### TVS-C.2 — Confirm TradingApp compile/verify/delete requirements and find minimal valid source

| Field | Value |
|-------|-------|
| **Type** | RESEARCH |
| **Effort** | M |
| **Goal** | Confirm that TradingApp uses the same compile/verify/delete flow as Strategy/Indicator/Function (no special CELCompiler fields, no different CreateEntryEX params, no type-specific dispatch), AND identify a minimal valid TradingApp EasyLanguage source for testing. |
| **Scope — DONE** | A findings document at `tmp/re_artifacts/tradingapp_compile_requirements.md` stating either (a) "TradingApp uses the standard flow — no changes needed" with evidence, or (b) "TradingApp requires these specific differences: ..." with a list. A minimal valid TradingApp EasyLanguage source string suitable for integration tests. |
| **Scope — NOT DONE** | No code changes. No CLI changes. No enum changes. Do NOT investigate other unconfirmed types. |
| **Acceptance criteria** | (1) Frida trace of TSDev.exe compiling/verifying a TradingApp, showing the CELCompiler fields (+0x10, +0x18, +0x28) and VerifyModule_New flag value match the confirmed values (0xa02, hwnd, 6, flag=1) — OR documented differences. (2) CreateEntryEX call for TradingApp observed with params (whType, 0, name, 0, 0, &errCode) matching the confirmed order — OR documented differences. (3) Delete orchestrator (FUN_101a8360) called with the TradingApp whType — OR documented differences. (4) A minimal EasyLanguage source that compiles successfully as a TradingApp in TSDev.exe (verified via Frida trace or TSDev UI). (5) Findings document exists with evidence. |
| **Dependencies** | TVS-C.1 (need the confirmed whType to trace/verify). |
| **Adversarial review** | YES — incorrect assumptions about compile requirements would cause TVS-C.4-C.6 to fail in subtle ways (e.g., silent compile failures, wrong CELCompiler fields). |

**Research approach:**

1. **Frida trace TSDev.exe creating + compiling a TradingApp**: Hook `CreateEntryEX`, `SaveModule_New`, `StartCompilingProcess`, `VerifyModule_New`, `GetCompilingErrors`, `EndCompilingProcess`, and the delete orchestrator `FUN_101a8360`. Create a TradingApp in the TSDev UI, compile it, then delete it. Capture all argument values. Compare against the confirmed parameter orders in `orlib_param_orders.md`.

2. **CELCompiler field inspection**: After the CELCompiler constructor is called for a TradingApp compile, read the object's fields at +0x10, +0x18, +0x28 to confirm they match the Strategy/Indicator/Function values (0xa02, hwnd, 6).

3. **Minimal source discovery**: Try creating a TradingApp with a minimal source in the TSDev UI. TradingApp is for "trading applications/gadgets" — it likely has different required keywords than Strategy (which uses Buy/Sell) or Indicator (which uses Plot). Research what a minimal valid TradingApp source looks like (may require TSDev UI experimentation, TradeStation documentation, or inspection of existing TradingApp sources in the library).

4. **Delete path confirmation**: Delete a TradingApp in the TSDev UI under Frida. Confirm FUN_101a8360 is called with ECX=TradingApp_whType, EDX=progId (same __fastcall convention as Strategy).

---

### TVS-C.3 — Add TradingApp to ComponentType enum and CLI type parsing

| Field | Value |
|-------|-------|
| **Type** | IMPLEMENTATION |
| **Effort** | S |
| **Goal** | Make TradingApp a recognized type in the codebase — the minimal code change to enable all downstream TVSs. |
| **Scope — DONE** | `ComponentType::TradingApp = <confirmed_whType>` added to the enum in `src/tsdev_lib.h`. The enum comment updated to reflect the new confirmed type. `"tradingapp"` added to `parseType()` in `src/tsdev_cli.cpp`. CLI usage strings updated to include `tradingapp`. Build succeeds. Existing tests (test_insert, test_delete, test_modify) still pass (regression check). |
| **Scope — NOT DONE** | No new test files. No new orlib20 calls. No changes to tsdev_lib.cpp logic. No documentation updates beyond the enum comment. |
| **Acceptance criteria** | (1) `cmake --build build --config Release` succeeds with no warnings or errors. (2) `tsdev_cli insert tradingapp foo bar.ela` is accepted (does not reject "tradingapp" as unknown type — it may fail later at the orlib level, but type parsing succeeds). (3) Existing test_insert, test_delete, test_modify all still pass (exit 0). (4) `git diff` shows changes only in `src/tsdev_lib.h` (enum + comment) and `src/tsdev_cli.cpp` (parseType + usage strings). |
| **Dependencies** | TVS-C.1 (need the confirmed whType integer for the enum value). |
| **Adversarial review** | YES — verify the enum value matches the confirmed whType exactly, no scope creep, no logic changes. |

**Implementation detail:**

In `src/tsdev_lib.h` L25-29, change:
```cpp
enum class ComponentType : int {
    Strategy  = 0,
    Indicator = 2,
    Function  = 4
};
```
to:
```cpp
enum class ComponentType : int {
    Strategy    = 0,
    Indicator   = 2,
    Function    = 4,
    TradingApp  = <CONFIRMED_WHTYPE>  // confirmed in TVS-C.1
};
```

In `src/tsdev_cli.cpp` L57-62, add:
```cpp
if (iequals(s, "tradingapp")) { out = ComponentType::TradingApp; return true; }
```

Update the usage strings in `printUsage()` and error messages in `cmdInsert`/`cmdDelete` to include `tradingapp`.

---

### TVS-C.4 — TDD red-green: TradingApp INSERT integration test

| Field | Value |
|-------|-------|
| **Type** | IMPLEMENTATION |
| **Effort** | S |
| **Goal** | Verify that INSERT (createEntry + saveSource + verifyModule + .ell persistence) works end-to-end for a TradingApp component. |
| **Scope — DONE** | New test file `src/test_insert_tradingapp.cpp` modeled on `src/test_insert.cpp`, using `ComponentType::TradingApp` and the minimal valid TradingApp source from TVS-C.2. Added to `CMakeLists.txt`. Test passes (exit 0) in 2 independent runs. Test is self-cleaning (deletes the .ell after the run). |
| **Scope — NOT DONE** | No changes to tsdev_lib.cpp logic. No DELETE or MODIFY tests (those are TVS-C.5 and TVS-C.6). No CLI changes beyond TVS-C.3. |
| **Acceptance criteria** | (1) `test_insert_tradingapp.cpp` compiles and links. (2) Test creates a TradingApp entry (createEntry returns ProgId > 0). (3) saveSource returns true. (4) verifyModule returns empty error string (compilation succeeds). (5) `.ell` file exists at `elmodule_<whType>_<progId>.ell` with magic `RNAM`. (6) Exit code 0 in 2 independent runs. (7) Test cleans up its .ell file. (8) `git diff` shows only the new test file and CMakeLists.txt addition. |
| **Dependencies** | TVS-C.1 (whType), TVS-C.2 (minimal valid source + compile requirements), TVS-C.3 (enum value). |
| **Adversarial review** | YES — verify the test is self-cleaning, uses the correct whType, does not modify the TS install, and the .ell file is genuinely a TradingApp (not accidentally a Strategy/Indicator). |

**TDD note:** This is the "green" step. The "red" was implicit: before TVS-C.3, `ComponentType::TradingApp` did not exist, so this test could not compile. After TVS-C.3, the test compiles. If INSERT fails at runtime (e.g., CreateEntryEX returns 0, or verifyModule returns errors), that indicates either an incorrect whType (TVS-C.1 finding was wrong) or a compile-requirement difference (TVS-C.2 finding was wrong). Diagnose and fix before proceeding.

**Test structure** (mirror `test_insert.cpp`):
- Test name: `zzz_cli_insert_tradingapp`
- Source: the minimal valid TradingApp source from TVS-C.2
- Flow: construct TsDevLib → findExistPos (skip if exists) → createEntry → saveSource → verifyModule → closeWarehouse → ellFileExists → cleanup
- Run on a 64 MB-stack worker thread (same as all other tests)

---

### TVS-C.5 — TDD red-green: TradingApp DELETE integration test

| Field | Value |
|-------|-------|
| **Type** | IMPLEMENTATION |
| **Effort** | S |
| **Goal** | Verify that DELETE (findExistPos + delete orchestrator + file removal) works end-to-end for a TradingApp component. |
| **Scope — DONE** | New test file `src/test_delete_tradingapp.cpp` modeled on `test_delete.cpp`, using `ComponentType::TradingApp`. Added to `CMakeLists.txt`. Test passes (exit 0) in 2 independent runs. Test is self-cleaning (creates a test entry, deletes it, verifies removal). |
| **Scope — NOT DONE** | No changes to tsdev_lib.cpp logic. No MODIFY tests (that is TVS-C.6). |
| **Acceptance criteria** | (1) Test creates a TradingApp entry (INSERT flow), verifies it exists. (2) Deletes it via `deleteEntry`. (3) `findExistPos` returns ≤0 after deletion. (4) `.ell` file is removed from disk. (5) `MWRKArea.wrk` timestamp changes (deletion persisted). (6) Exit code 0 in 2 independent runs. (7) Negative test: deleting a nonexistent TradingApp returns false. (8) `git diff` shows only the new test file and CMakeLists.txt addition. |
| **Dependencies** | TVS-C.3 (enum value), TVS-C.4 (INSERT must work first — DELETE test needs to create a test entry to delete). |
| **Adversarial review** | YES — verify deletion persists to MWRKArea.wrk, .ell is removed, no WerFault/crashes, negative test works. |

**Test structure** (mirror `test_delete.cpp`):
- Test name: `zzz_cli_delete_tradingapp`
- Flow: INSERT test entry → verify exists → deleteEntry → verify not found → verify .ell removed → negative test (delete nonexistent)
- Run on a 64 MB-stack worker thread

---

### TVS-C.6 — TDD red-green: TradingApp MODIFY integration test

| Field | Value |
|-------|-------|
| **Type** | IMPLEMENTATION |
| **Effort** | S |
| **Goal** | Verify that MODIFY (findExistPos + saveSource + verifyModule on existing ProgId) works end-to-end for a TradingApp component. |
| **Scope — DONE** | New test file `src/test_modify_tradingapp.cpp` modeled on `test_modify.cpp`, using `ComponentType::TradingApp`. Added to `CMakeLists.txt`. Test passes (exit 0) in 2 independent runs. Test is self-cleaning. |
| **Scope — NOT DONE** | No changes to tsdev_lib.cpp logic. No CLI wiring of modify subcommand (that is a separate concern — see §6 Risk R6). |
| **Acceptance criteria** | (1) Test creates a TradingApp entry with source A (INSERT flow). (2) `modifyEntry` with source B returns `Result{success=true}`. (3) ProgId is preserved (same before and after). (4) `.ell` file timestamp changes (re-compiled + re-persisted). (5) Negative test: `modifyEntry` on a nonexistent TradingApp returns `Result{success=false}`. (6) Compile-error test: `modifyEntry` with invalid source returns `Result{success=false}` with non-empty message. (7) Exit code 0 in 2 independent runs. (8) Cleanup: test entry is deleted after the run. (9) `git diff` shows only the new test file and CMakeLists.txt addition. |
| **Dependencies** | TVS-C.3 (enum value), TVS-C.4 (INSERT must work — MODIFY test needs to create a test entry to modify). |
| **Adversarial review** | YES — verify ProgId preservation, .ell update, negative test, compile-error test, cleanup. |

**Test structure** (mirror `test_modify.cpp`):
- Test name: `zzz_cli_modify_tradingapp`
- Source A: minimal valid TradingApp source (from TVS-C.2)
- Source B: a different valid TradingApp source (e.g., different variable/logic)
- Flow: INSERT source A → record ProgId + .ell timestamp → modifyEntry(source B) → verify success → verify ProgId unchanged → verify .ell timestamp changed → negative test → compile-error test → cleanup (deleteEntry)
- Run on a 64 MB-stack worker thread

---

### TVS-C.7 — Update documentation to reflect confirmed TradingApp whType

| Field | Value |
|-------|-------|
| **Type** | IMPLEMENTATION |
| **Effort** | S |
| **Goal** | Update all project documentation to reflect the confirmed TradingApp warehouse type and its supported status. |
| **Scope — DONE** | `docs/FEASIBILITY.md` §5.2 updated: TradingApp row changed from UNRESOLVED to CONFIRMED with the whType value and evidence reference. `docs/TS_COMPONENT_TYPES.md` updated: mapping table and open questions updated. `AGENTS.md` TODO updated: "Unresolved warehouse-type mappings" follow-up updated to reflect TradingApp is now resolved (4 remaining: ActivityBar, PaintBar, ProbabilityMap, ShowMe). `src/tsdev_lib.h` enum comment updated (if not already done in TVS-C.3). `tmp/re_artifacts/orlib_param_orders.md` §1 updated: TradingApp moved from UNRESOLVED to CONFIRMED. |
| **Scope — NOT DONE** | No code changes. No test changes. Do NOT claim the other 4 unconfirmed types are resolved. |
| **Acceptance criteria** | (1) All 5 documents above consistently show TradingApp as CONFIRMED with the same whType value. (2) The 4 remaining unconfirmed types (ActivityBar, PaintBar, ProbabilityMap, ShowMe) are still listed as UNRESOLVED. (3) No contradictions between documents. (4) `git diff` shows only documentation/comment changes. |
| **Dependencies** | TVS-C.4, TVS-C.5, TVS-C.6 (all tests must pass before declaring TradingApp "supported"). |
| **Adversarial review** | YES — verify all documents are consistent, no overclaiming, remaining unconfirmed types still marked as such. |

---

## 5. Research Interleaving

The plan follows a strict research-first ordering:

```
TVS-C.1 (RESEARCH: discover whType)
    ↓
TVS-C.2 (RESEARCH: confirm compile/verify requirements + minimal source)
    ↓
TVS-C.3 (IMPLEMENTATION: add enum + CLI parsing)
    ↓
TVS-C.4 (IMPLEMENTATION: INSERT test — red-green)
    ↓
TVS-C.5 (IMPLEMENTATION: DELETE test — red-green)
    ↓
TVS-C.6 (IMPLEMENTATION: MODIFY test — red-green)
    ↓
TVS-C.7 (IMPLEMENTATION: documentation update)
```

**Why research first:** The TradingApp whType is the single critical unknown. Every implementation TVS depends on it. If the whType is wrong, `CreateEntryEX` will create a component of the wrong type (or fail), `FindExistPos` will not find TradingApp components, and the delete orchestrator will target the wrong warehouse partition. The whType must be confirmed and adversarially reviewed before any code changes.

**Why TVS-C.2 is separate from TVS-C.1:** Discovering the whType (TVS-C.1) is a different research question from confirming the compile/verify flow (TVS-C.2). TVS-C.1 can use static analysis and file scanning without launching TSDev.exe. TVS-C.2 requires a live Frida trace of TSDev.exe compiling a TradingApp, which depends on knowing the whType from TVS-C.1. Splitting them keeps each research task narrowly scoped (per AGENTS.md: "One subagent → one narrowly scoped subtask").

**Gate between research and implementation:** TVS-C.3 (the first implementation TVS) must NOT start until both TVS-C.1 and TVS-C.2 are adversarially APPROVED. If TVS-C.2 reveals that TradingApp requires a different compile flow, additional implementation TVSs may need to be inserted between TVS-C.3 and TVS-C.4 (the plan would need to be revised by the team at that point).

---

## 6. Risk Register

| ID | Risk | Likelihood | Impact | Mitigation |
|----|------|-----------|--------|------------|
| R1 | **TradingApp whType is not a simple integer** (e.g., it requires a composite ID like subtype<<8 \| whType, or a different numbering scheme than 0-4). | Low | High | TVS-C.1 uses multiple methods including Frida trace of the authoritative caller (TSDev.exe), which will capture the exact value passed. If a composite ID is needed, `CreateEntryEX` and `FindExistPos` still take an integer — the enum value would be the composite ID. |
| R2 | **TradingApp has special compile/verify requirements** (different CELCompiler fields, different VerifyModule_New flag, or a completely different compile path). | Medium | High | TVS-C.2 explicitly traces the compile/verify flow under Frida. If differences are found, they are documented and the implementation TVSs are adjusted. The existing code sets CELCompiler fields to fixed values (0xa02, hwnd, 6) — if TradingApp needs different values, `verifyModule()` may need a type-specific branch. |
| R3 | **TradingApp cannot be created headlessly** (CreateEntryEX may reject the whType, or VerifyModule_New may require a GUI element specific to TradingApp). | Low | High | TVS-C.4 is the gate. If INSERT fails, diagnose via Frida: compare the headless CLI's orlib20 calls against TSDev.exe's live calls for the same TradingApp. The persistence patch and thread-check patches are type-agnostic, so they should not be affected. |
| R4 | **No TradingApp components exist in the library**, making the .ell file scan method (TVS-C.1 method 1) uninformative. | Medium | Low | The .ell scan is only one of 6 proposed methods. The Frida trace (method 2) and static analysis (methods 3-4) do not depend on existing TradingApp components. The user may need to create a TradingApp in the TSDev UI for the Frida trace. |
| R5 | **TradingApp has EasyLanguage constraints** (like Strategy's no-Plot rule) that make the "minimal valid source" non-obvious. | Medium | Medium | TVS-C.2 includes discovering a minimal valid source. If TradingApp requires specific keywords or structures, the test source must include them. The test is expected to fail on invalid source (which is itself a valid test of the compile-error path). |
| R6 | **CLI does not expose a `modify` subcommand** — `modifyEntry` exists in the library but `tsdev_cli.cpp`'s `main()` only dispatches `insert` and `delete`. TradingApp MODIFY would work at the library level (TVS-C.6) but not via the CLI. | High | Low | This is a pre-existing gap, not specific to TradingApp. TVS-C.6 tests MODIFY at the library level (matching the existing `test_modify.cpp` pattern). Wiring `modify` into the CLI is a separate follow-up outside this workstream's scope. Note this in the handoff but do not expand scope. |
| R7 | **TradingApp whType collides with an existing type** (e.g., TradingApp = 1, which is listed as "Function" in the static survey but "Function" is confirmed as 4). | Low | High | The static survey's whType 1 = "Function" is UNCONFIRMED (only whType 4 = Function is confirmed). If TradingApp = 1, this resolves the ambiguity (1 = TradingApp, not Function). TVS-C.1's cross-validation requirement (2+ methods) guards against collisions. |
| R8 | **The delete orchestrator (FUN_101a8360) behaves differently for TradingApp** — it contains strategy-specific code paths (e.g., `FUN_101c09f0(0xb)` and `FUN_101c09f0(0xc)` are described as "for strategy types" in the code comments at `tsdev_lib.cpp` L536). | Medium | Medium | TVS-C.2 traces the delete path under Frida. TVS-C.5 verifies deletion end-to-end. If the orchestrator does not handle TradingApp, a different delete approach may be needed (the team should diagnose and plan at that point). |
| R9 | **TradingApp requires additional support DLLs** not in the current pre-load list (`ECL.dll`, `ORELUI20.dll`, `orUI20.dll`, `elobjmod.dll`, `elwinforms.dll`, `tschartxelobjmdl.dll`, `ELDebugger.dll`). | Low | Medium | TVS-C.2's Frida trace will reveal if TSDev.exe loads additional DLLs when compiling a TradingApp. If so, they must be added to the pre-load list in `tsdev_lib.cpp`. |
| R10 | **TradingApp .ell files use a different filename pattern** (not `elmodule_<whType>_<progId>.ell`). | Low | Medium | TVS-C.1 method 1 (file scan) and TVS-C.2 (Frida trace of SaveModule_New/VerifyModule_New) will reveal the actual filename pattern. The `getEllPath()`/`getXmlPath()` functions in `tsdev_lib.cpp` use the `elmodule_%d_%d.ell` pattern — if TradingApp differs, these functions need adjustment. |

---

## 7. Constraints

1. **Standalone operation** — No TSDev.exe required for the CLI itself. Research TVSs (C.1, C.2) MAY use TSDev.exe under Frida for tracing (this is read-only observation, not a runtime dependency). Implementation TVSs (C.3-C.7) must work without TSDev.exe running.

2. **No WHServer killing** — The CLI must not kill `WHServer.exe` or any TradeStation process. The thread-check INT3 patches (already applied in the `TsDevLib` constructor) handle the thread-affinity issue without process termination.

3. **READ-ONLY TradeStation install** — `C:\Program Files (x86)\TradeStation 9.5` is never modified. All binary patches (persistence patch, thread-check patches) are applied in-process to the loaded DLL image (memory only). The on-disk `orlib20.dll` is untouched. Files from the TS install that need inspection are copied into `tmp/` first.

4. **32-bit** — All builds target Win32 (x86). `CMakeLists.txt` enforces this with a `CMAKE_SIZEOF_VOID_P == 4` guard. orlib20.dll is a 32-bit PE.

5. **Adversarial review required** — Every TVS result (research findings, code changes, test results) must be adversarially reviewed by an independent subagent per AGENTS.md. The reviewer receives the original TVS instructions and the result, and checks for complete execution, no scope creep, no drift, no undue shortcuts. APPROVE/DISAPPROVE loop continues until APPROVED.

6. **TDD where applicable** — Implementation TVSs follow the red-green method. TVS-C.4, C.5, C.6 each write integration tests that verify the full flow end-to-end. Tests are self-cleaning (delete test artifacts) so they can be repeated idempotently.

7. **One subagent → one narrowly scoped subtask** — Each TVS is executed by a single focused subagent. No multi-stage or multi-concern subtasks. If a TVS reveals additional work is needed, a new TVS is defined rather than expanding the current one.

8. **Frida usage** — Use Frida DIRECTLY via the `frida` CLI (`frida-trace`, `frida -l script.js`, `frida -p PID`). NEVER via MCP tools (the Frida MCP server was removed). Frida 17.17.0 is installed. Live RE works: 32-bit spawn/attach, interactive sessions, full Frida JS API.

9. **Ghidra usage** — Ghidra MCP tools ARE available (server name: "ghidra"). Use for static analysis (decompilation, disassembly, symbol resolution, cross-references).

10. **Do not claim unconfirmed types** — Only TradingApp is being confirmed in this workstream. The other 4 unconfirmed types (ActivityBar, PaintBar, ProbabilityMap, ShowMe) must remain marked as UNRESOLVED in all documentation. Do not speculate about their whType values.

---

## 8. Execution Notes for the Fresh Team

1. **Start by reading this document + `AGENTS.md` + `src/tsdev_lib.h` + `src/tsdev_cli.cpp`.** These give you the full context.
2. **Determine current project state** — check `git log` and `AGENTS.md` TODO for any progress since this document was written. If TVS-C.1 is already done, start from TVS-C.2.
3. **Execute one TVS at a time.** Each TVS is minimal and independently verifiable. Do not skip ahead.
4. **Research TVSs (C.1, C.2) produce findings documents in `tmp/re_artifacts/`.** Implementation TVSs (C.3-C.7) produce code/test/doc changes.
5. **Every TVS must be adversarially reviewed before proceeding to the next.**
6. **If a TVS fails** (e.g., TVS-C.4 INSERT test fails), diagnose the root cause. It likely means either TVS-C.1 (whType) or TVS-C.2 (compile requirements) had an incorrect finding. Go back and re-examine, then re-run the failed TVS.
7. **If TVS-C.2 reveals TradingApp needs a different compile flow**, stop and revise this plan before proceeding to TVS-C.3. The implementation TVSs may need adjustment (e.g., type-specific CELCompiler fields in `verifyModule()`).
8. **Build commands**: `cmake -S . -B build -A Win32` then `cmake --build build --config Release`. See `docs/BUILD.md`.
9. **Test execution**: Run from the `build\Release\` directory. Tests require no TradeStation process running (headless). Each test runs its orlib20 flow on a 64 MB-stack worker thread.
10. **File naming for test artifacts**: Test component names should use the `zzz_cli_*` prefix (matching existing tests) to avoid collisions with real components.
