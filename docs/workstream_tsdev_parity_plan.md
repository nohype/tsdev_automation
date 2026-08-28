# Workstream M — TSDev Verify/Library Parity Follow-ups

**Status:**
- M.1 (CELCompiler allocation/setup/destruction parity) — COMPLETE, APPROVED; see `docs/M1_HANDOFF.md`.
- M.2 (`_SaveModule_New` flags + CELProperties parity) — COMPLETE, APPROVED; see `docs/M2_HANDOFF.md`.
- M.3 (post-success `0x40A` notification) — COMPLETE, APPROVED; see `docs/M3_HANDOFF.md`.
- M.3b.1 (object-code version diagnosis) — COMPLETE, APPROVED; see `docs/RE/RE_M3B_OBJECT_CODE_VERSION_DEFECT.md`.
- M.3b.2a (Phase 1 high-level path comparison) — COMPLETE, APPROVED.
- M.3b.2b (Phase 2 dynamic investigation) — COMPLETE, APPROVED; the M.3b "defect" is a test artifact (declaration-free source legitimately has elemCount=0).
- M.3b.2c (Phase 3: fix RED test + close sample-size gap) — COMPLETE, APPROVED; test remains a manual admin diagnostic, not wired into regression.
- M.4a (RE: resolve CELPrograms BST population source) — COMPLETE, APPROVED; warehouse provides reverse dependents via cmd `0x3B` (`IProgram::Callers`); see `docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md` §9.
- M.4b.1 (warehouse-level enumeration primitive) — RE-OPENED; see §M.4b.1 below and `docs/M4B1_HANDOFF.md`.
- M.4b.2 (wire into revalidation + fix blockers + remove linker code) — pending.
- M.5 (consolidated parity/runtime verification + documentation closure) — pending.

**Review status:** All completed slices passed independent + adversarial review. Historical review does not establish current acceptance.

**Priority:** M.4b.1 is the active TVS — 13 missing warehouse commands identified via Frida trace of TSDev.exe; next step is Ghidra-analyze them to find which populate the dependency DB. M.4b.2 follows M.4b.1; M.5 follows M.4b.2.

**Dependency on L:** Workstream L is closed; the L dependency is satisfied. M.1–M.3b.2c are complete historical slices.

**Source authority (approved findings only):**
- `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md` — root cause, production fix design, §L follow-up parity section.
- `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md` — static appendix (call chains, ABI, COM ordering, loader GUID check).
- `docs/PM_WORKING_MEMORY.md` §§2–4 — current workstream status, gates, audit ledger.
- `AGENTS.md` — emulate-TSDev rule, no-hacks rule, no-source-text-metadata rule, TDD/TVS/adversarial-review rules, mandatory CLI E2E rule.
- `docs/TS_COMPONENT_TYPES.md` — confirmed warehouse type mappings for the five supported component types.

**Durable-evidence prerequisite — SATISFIED:** Former `tmp/unknown_link_*` artifacts are durably promoted to `docs/RE/`. M slices must cite `docs/RE/` paths, not former `tmp/` paths. Raw `tmp/` artifacts (Frida traces, probe stdout) are transient/reproducible, not authoritative.

## 1. Goals, non-goals, confirmed facts, open questions

### Goals
- Bring CLI/library behavior closer to TSDev.exe emulation for the four parity concerns in §L of the root-cause plan, plus a closure slice consolidating verification and documentation across all five supported component types and operation modes.
- Each M.1–M.5 is an independent thin vertical slice (TVS): one narrowly scoped concern, TDD red→green, mandatory real CLI E2E, adversarial review, handoff.
- Future teams executing a slice may perform slice-scoped research (Ghidra/Frida) bounded to that slice.

### Non-goals (hard)
- M is **not** the confirmed immediate root cause (that is Workstream L). No M item may be presented as a root-cause fix.
- No source-text metadata parsing. Names/types/ProgIds come from orlib20 warehouse records/APIs, never item source content.
- No user-facing password protection (internal encryption only; user always sees plain text).
- No bypass restoration (`FUN_10011230`).
- No new RE of unrelated orlib20 subsystems; slice-scoped RE only where delegated.
- No modification of the TradeStation installation directory (read-only).

### Confirmed facts (baseline for all M slices)
- TSDev UI Verify command chain: `WM_COMMAND 0x296A → FUN_0047B0E0 → FUN_00451800 → FUN_004D9480 → FUN_004D91E0 → orlib20!_VerifyModule_New@20` (`docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md` §6).
- TSDev UI Verify flow ordering: save source → allocate/configure 0x58-byte `CELCompiler` → `StartCompilingProcess` → `_VerifyModule_New@20` → dependent-program handling → `EndCompilingProcess` → success event `0x40A` → properties/view refresh → compiler destruction.
- TSDev configures `CELCompiler`: `+0x08`=category, `+0x0C`=subtype, `+0x10`=current ProgId, `+0x18`=notification HWND; clears `+0/+4` before compilation.
- Post-success `0x40A` is live-only for already-running clients; not the durable link fix.
- Reverse-dependent revalidation propagates signature/contract changes to callers; not required to make the root Strategy itself linkable.
- Non-NULL password in `_SaveModule_New` controls source encryption/protection, not final linking; not a user password feature.
- Runtime loader uses `.ell` with GUID validation; XML is debugger/source-map only.
- Five supported component types and warehouse mappings: Strategy (whType 0), Indicator (whType 2), Function (whType 4), TradingApp (whType 5122), ShowMe (whType 1282). whType = `(subType << 8) | category` (`docs/TS_COMPONENT_TYPES.md`).

### Open implementation questions
Each is listed under its owning slice below. None is pre-answered here; each future TVS team owns its slice's research.

---

## 2. Slice specifications (M.1–M.5)

### M.1 — Exact CELCompiler allocation/setup/destruction parity — COMPLETE
APPROVED historical slice; see `docs/M1_HANDOFF.md`.

### M.2 — `_SaveModule_New` flags and CELProperties parity — COMPLETE
APPROVED historical slice; see `docs/M2_HANDOFF.md`.

### M.3 — Post-success `0x40A` notification — COMPLETE
APPROVED historical slice; see `docs/M3_HANDOFF.md`.

### M.3b — Object-code version "defect" — COMPLETE (test artifact)
The M.3b "defect" (elemCount=0 for CLI-compiled items) was a test artifact: the RED test used declaration-free source which legitimately has elemCount=0. CLI-compiled items WITH declarations produce non-zero elemCount. No production compilation-path fix was needed. The test remains a manual admin diagnostic (not wired into regression). See `docs/RE/RE_M3B_OBJECT_CODE_VERSION_DEFECT.md` §12-13.

### M.4 — Reverse-dependent revalidation equivalent to TSDev `FUN_004D91E0`

**Objective:** Emulate TSDev's dependent-program traversal at the highest possible abstraction level — using warehouse-level APIs TSDev.exe itself uses, not parsing orlib20 internal buffer layouts. When a component changes its signature/contract, its reverse dependents/callers are revalidated, with contract-change detection and recursion safety.

**Re-scoping rationale:** The original linker-based approach (ordinal 85 + brute-force scan + manual parsing of orlib20 internal buffer layouts) is at the wrong abstraction level — it parses structures TSDev.exe never touches directly. TSDev.exe uses a `CELPrograms` BST populated from warehouse SafeArray data via warehouse commands. M.4 is split into M.4a (RE) + M.4b (implementation). The existing linker-based `enumerateReverseDependents` is retained as historical evidence until M.4b replaces it; it is NOT a current acceptance result.

**Three pre-existing M.4 blocker escalations** (see `AGENTS.md §"M.4 blocker escalations"`; apply regardless of enumeration method, addressed in M.4b.2):
1. Dependent revalidation flag: `callVerify()` hardcodes flag=1 instead of 0 for dependents.
2. Old master-record capture: `saveSource()` never captures the old record, leaving `contractChanged=true` by default.
3. Cycle safety: `traverseReverseDependents()` doesn't establish safe termination when `contractChanged=true`.

#### M.4a — RE: Resolve CELPrograms BST population source — COMPLETE
APPROVED on iteration 2 (2026-08-28). The warehouse provides reverse dependents directly via cmd `0x3B` (`IProgram::Callers`). See `docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md` §9.

#### M.4b — Implementation: Warehouse-level reverse-dependent revalidation

Split into M.4b.1 (enumeration primitive) + M.4b.2 (wire into revalidation + fix blockers + remove linker code).

**Abstraction-level assessment (2026-08-28):** M.4a §9 confirmed TSDev.exe drives cmd 0x0D → cmd 0x3B through the warehouse worker protocol — the same `FUN_101cf5b0` protocol the CLI already uses for cmd 0x4E. There is no higher-level exported API. The warehouse-level approach IS the highest possible abstraction level — exactly what TSDev.exe does. The linker-based approach was the wrong (too-deep) level.

##### M.4b.1 — Warehouse-level reverse-dependent enumeration primitive (RE-OPENED — incomplete)

**Objective:** Implement and prove the warehouse-level `IProgram::Callers` enumeration (cmd 0x0D → cmd 0x3B → SafeArray parse) as a standalone, tested primitive. This is the core mechanism M.4b needs, extracted as a focused TVS to de-risk the warehouse COM protocol before wiring it into revalidation.

**Known factual baseline (from M.4a §9, static-only — dynamic confirmation is part of this TVS's RED→GREEN):**
- The warehouse singleton is obtained via `resolveGetWarehouse()` (signature `GetWarehouse`, expectedRva `0x1A9650`); the CLI already calls it in `refreshCache` and `enumerateRecords` Phase 2.
- The generic command dispatcher `FUN_101cf5b0` is already resolved (`m_pCmdDispatcher`, via `resolvePatchSite("FUN_101cf5b0_thread_check_INT3")`) and callable via `callCmdDispatcher(m_pCmdDispatcher, warehouse, cmdId)` — used today for cmd 0x4E in `refreshCache`. This is the SAME protocol TSDev.exe uses (`FUN_004d6f00`).
- cmd 0x0D sequence (per §9.1/§9.4): set `warehouse+0x12 = type`, `warehouse+0x16 = ProgId`, then `callCmdDispatcher(warehouse, 0x0D)`, read result CELProgram record from `warehouse+0x5e`.
- cmd 0x3B sequence (per §9.2/§9.4): the current record must be set at `warehouse+0x5e` (done by cmd 0x0D), then `callCmdDispatcher(warehouse, 0x3B)`, read SafeArray from `warehouse+0x26`.
- SafeArray entry format (per §9.7): stride `8 + nameLength + 0x16`; header `+0x00` 2-byte type/category, `+0x02` 4-byte ID-like field (likely ProgId), `+0x06` 2-byte name length, then name bytes, then `0x16`-byte metadata block.
- Guard: after cmd 0x3B, check `warehouse+0x4e == 0` (status/error) before reading the SafeArray.
- Thread affinity: cmd 0x0D and cmd 0x3B must be issued from the warehouse-owning thread (the CLI already satisfies this for cmd 0x4E).

**Research questions delegated to this TVS (slice-scoped, resolved by RED→GREEN):**
- R-M.4b.1.1: Does the CLI's existing `_GetMasterRecByType@16` call leave the CELProgram pointer at `warehouse+0x5e` (enabling a direct cmd 0x3B follow-up), or must M.4b.1 issue a raw cmd 0x0D via `callCmdDispatcher(warehouse, 0x0D)` to set `warehouse+0x5e`? Determine empirically via the RED test; prefer the raw cmd 0x0D path if it most closely matches TSDev's `FUN_004d7040`.
- R-M.4b.1.2: Confirm the SafeArray entry `+0x02` "ID-like field" is the reverse-dependent's ProgId (via a known caller's ProgId from `exists`).
- R-M.4b.1.3: Confirm the `+0x00` 2-byte field maps to a category/whType that the CLI can resolve to a `ComponentType` (cross-reference with `docs/TS_COMPONENT_TYPES.md`).
- R-M.4b.1.4: If the static offsets are wrong, use Frida (read-only, elevated) to instrument `FUN_004cad90` or the orlib20 worker cmd 0x3B handler during a live TSDev verify and correct the offsets. Record findings in the handoff.

**Implementation boundary:**
- In scope: a new `enumerateCallers(ComponentType type, const std::string& name)` method on `TsDevLib` that drives cmd 0x0D → cmd 0x3B → SafeArray parse and returns a vector of reverse dependents (name, type/category, ProgId — all warehouse-derived). A new read-only `dependents <type> <name>` CLI subcommand with `--json` and human output, structured like `exists`. A RED→GREEN test.
- Out of scope: revalidation traversal (`traverseReverseDependents`), the three blocker escalations, contract-change detection, removing the linker-based `enumerateReverseDependents` code, root Strategy linkability (L), compiler setup (M.1), save flags (M.2), `0x40A` (M.3), source-text parsing, user password.
- Must not modify `traverseReverseDependents`, `callVerify`, `saveSource`, or `verifyModule` — those are M.4b.2.
- Must not remove or alter the linker-based `enumerateReverseDependents` — M.4b.1 adds the new primitive alongside it; M.4b.2 removes the old code.
- Must not introduce source-text-based dependency detection. Reverse dependents come from the warehouse `IProgram::Callers` SafeArray.

**Likely files/systems:**
- `src/tsdev_lib.cpp` (new `enumerateCallers` method; warehouse cmd 0x0D/0x3B driving via existing `callGetWarehouse` + `callCmdDispatcher`; SafeArray parse).
- `src/tsdev_lib.h` (new method declaration + result struct).
- `src/tsdev_cli.cpp` (new `dependents` subcommand dispatch + `cmdDependents` + `doDependents` + `runDependents` worker thread, modeled on `exists`).
- `tests/test_callers_enumeration.cpp` (new RED→GREEN test; live TS-dependent; manual admin target in CMake, NOT in regression — same pattern as `test_m3b_object_code_version`).
- No `signatures/*.json` or `docs/SIGNATURE_CATALOG.md` changes expected (`FUN_101cf5b0` is already a patch site; `GetWarehouse` is already a signature). If a new signature is needed, update the SSOT per §5.

**TDD plan (red → green):**
1. RED: Write `tests/test_callers_enumeration.cpp` that: (a) inserts a disposable Function with `Inputs:` declarations, (b) inserts a disposable caller (Indicator or Strategy) that references the Function, (c) calls `lib.enumerateCallers(ComponentType::Function, funcName)` and asserts the result contains the caller with the correct name and ProgId (from `exists`). This fails because `enumerateCallers` does not exist yet. Gate the test on TS presence (construct `TsDevLib`/`OrlibResolver` in try/catch, exit early if unavailable — same pattern as `test_m3b_object_code_version.cpp`). Run on a 64 MB-stack worker thread. Clean up all disposable items.
2. GREEN: Implement `enumerateCallers` per the M.4a §9 sequence. The test passes.
3. If the static offsets are wrong (RED fails unexpectedly), use Frida read-only to correct them (R-M.4b.1.4). Record the correction in the handoff note and update `docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md` §9.
4. Add the `dependents` CLI subcommand and verify it returns the same result as the test via a real CLI invocation.

**Acceptance criteria:**
- `enumerateCallers(type, name)` returns the correct reverse-dependent set for a live warehouse with a known Function→caller relationship, with warehouse-derived name and ProgId (no source-text parsing).
- The `dependents <type> <name>` CLI subcommand produces correct human and `--json` output (including `stored_name`).
- The cmd 0x0D → cmd 0x3B → SafeArray mechanism is confirmed dynamically (the RED→GREEN test IS the dynamic verification that M.4a §9.8 gap #2 flagged as missing).
- No existing operation (insert/modify/delete/exists/get-source/upsert) is broken — confirmed by the post-TVS E2E gate.
- No source-text parsing, no password fiction, no scope creep into revalidation/blockers/linker-removal, no root-cause conflation.
- Adversarial review APPROVED.

> **Session 3 update (2026-08-28):** Two earlier hypotheses disproved: warehouse+0xAE is NON-ZERO (0x009560F4) and cmd 0x0F IS dispatched during our CLI's verify (Frida confirmed FUN_101d1010 fires multiple times). The empty DB is NOT caused by either. **Breakthrough:** Frida trace of TSDev.exe verify (driven by reusable pywinauto UI automation, `uv run tsdev-ui`) reveals 13 warehouse commands TSDev sends that our CLI never sends: cmds 0x32/0x33/0x34/0x35/0x38/0x3A/0x4D/0x4E/0x4F/0x53/0x57/0x58/0x65. These commands call vtable methods on `param_1[7].Release` (same interface as cmd 0x3B, which our CLI does dispatch; cmd 0x0D uses `param_1[0xe]`). Next step: Ghidra-analyze these 13 commands' vtable methods to identify which one(s) populate the dependency database, then replicate them in our CLI. See `docs/M4B1_HANDOFF.md` §4.6/§4.7/§10 and `tmp/M4B1_FINDINGS.md` F5-F8.

**Mandatory CLI E2E:**
- Run the real CLI E2E per `docs/E2E_TEST_SPEC.md` insert/modify/delete for at least Strategy and Function from an elevated direct CLI. Confirm no regression from the new code. This is the standard post-TVS gate (the new `dependents` subcommand is additive and does not alter existing operations).
- Additionally, verify `dependents <type> <name>` returns the expected caller for a disposable Function→Indicator pair created during the E2E, then clean up.

**Review/handoff requirements:**
- Adversarial review: confirm the implementation uses the warehouse-level cmd 0x0D/0x3B approach (not linker-based), no source-text parsing, no scope creep into M.4b.2 concerns, the RED→GREEN is real (not a mock), complete execution.
- Handoff note recording: confirmed offsets (correcting any M.4a §9 static errors), the SafeArray entry field semantics (R-M.4b.1.2/R-M.4b.1.3), whether raw cmd 0x0D or `_GetMasterRecByType@16` side-effect was used (R-M.4b.1.1), and any Frida corrections.

##### M.4b.2 — Wire warehouse-level enumeration into revalidation + fix blockers + remove linker code (pending M.4b.1 completion)

**Objective:** Wire the M.4b.1-confirmed `enumerateCallers` into `traverseReverseDependents`, implement contract-change detection (`FUN_004d9040` field offsets, §9.6), implement recursion/cycle safety, address the three M.4 blocker escalations, remove the linker-based code, and run the full dependency-graph + operation-mode gates.

**Implementation boundary:**
- In scope: Replace `enumerateReverseDependents` calls in `traverseReverseDependents` with `enumerateCallers`. Implement contract-change detection (warehouse/metadata-derived only, §9.6 offsets). Implement recursion/cycle safety. Implement per-dependent failure reporting. Address the three blocker escalations: (1) `callVerify` passes flag=0 for dependents, (2) `saveSource` captures the old master record, (3) cycle safety is proven. Remove the linker-based `enumerateReverseDependents` code and its internal buffer layout parsing.
- Out of scope: root Strategy linkability (L), compiler field setup (M.1), save flags (M.2), `0x40A` (M.3), source-text parsing, user password.
- Must not introduce source-text-based dependency detection.

**Likely files/systems:**
- `src/tsdev_lib.cpp` (replace `enumerateReverseDependents` usage in `traverseReverseDependents`; fix `callVerify` flag; add old-master-record capture to `saveSource`; remove linker-based code).
- `src/tsdev_lib.h` (member declarations).
- Tests: `tests/test_reverse_dependent_revalidation.cpp` (update for new approach), E2E.

**TDD plan (red → green):**
1. RED: Add a test that inserts a Function with declarations, inserts a caller (Strategy/Indicator) that calls the Function, modifies the Function's signature, and asserts caller revalidation via the new warehouse-level approach. This fails because the current implementation uses the linker-based approach.
2. GREEN: Wire `enumerateCallers` into `traverseReverseDependents`; implement contract-change detection and cycle safety; fix the three blockers. The test passes.
3. Required regression: use a synthetic dependency enumerator/graph to prove repeated nodes and cycles cannot hang or stack-overflow. Add a live warehouse cycle only if R-M.4.3 confirms such a graph is valid and can be created through supported APIs. Per-dependent failures must report warehouse-derived name and ProgId, and successful acyclic revalidation must complete.
4. Remove the linker-based `enumerateReverseDependents` code and its internal buffer layout parsing (element arrays, dep-lists at `element+0x7b`, `0xFA3` entry parsing).

**Acceptance criteria:**
- Reverse dependents of a changed component are revalidated per TSDev's confirmed traversal, using the warehouse-level approach (not linker-based internal buffer parsing).
- Contract-change detection is warehouse/metadata-derived; no source-text parsing.
- Recursion/repeated-node handling is proven safe with a synthetic graph; live-cycle behavior is tested only if supported warehouse/API construction is confirmed.
- Per-dependent failures are reported with dependent identity (name, ProgId from warehouse).
- The three M.4 blocker escalations are resolved: (1) `callVerify` passes flag=0 for dependents, (2) `saveSource` captures the old master record, (3) cycle safety is proven.
- No linker-based internal buffer layout parsing remains in production code.
- No scope creep, no password fiction, no root-cause conflation.

**Mandatory CLI E2E:**
- Real CLI E2E per `docs/E2E_TEST_SPEC.md` plus a dependency-graph scenario: insert Function (with declarations) → insert caller (with declarations) → modify Function signature → assert caller revalidation outcome. Confirm the caller remains runtime-linkable in Desktop after successful revalidation (Strategy gate). E2E must pass before M.4b.2 is considered done.
- **Operation-mode coverage (do not defer to M.5):** Cover at least one representative reverse-dependency graph in both (a) manifest/stdin batch mode and (b) serve mode. Assert: revalidation runs per operation, no visited-set/cycle-state leaks across operations, no stale traversal state.

**Review/handoff requirements:**
- Adversarial review: confirm the implementation uses the warehouse-level approach (not linker-based), recursion safety, no source-text parsing, failure reporting completeness, the three blocker escalations are resolved, no scope creep, complete execution.
- Handoff note recording the confirmed traversal, contract-change detection mechanism, cycle handling, failure reporting semantics, and the blocker escalation resolutions.

---

### M.5 — Consolidated parity/runtime verification and documentation closure (PENDING)

**Objective:** Consolidate parity and runtime verification across all five supported component types (Strategy, Indicator, Function, TradingApp, ShowMe) and all operation modes (single CLI, manifest/stdin batch, directory batch, serve single, Python batch-over-serve), and close documentation gaps opened by M.1–M.4.

**Implementation boundary:**
- In scope: consolidated E2E matrix execution, residual-gap catalog, documentation closure (correcting stale statements, adding M.1–M.4 outcomes), final runtime gate.
- Out of scope: new parity concerns beyond cataloging (open follow-up slices instead), root-cause changes, source-text parsing, password fiction.
- Must not retroactively edit history silently; prepend dated corrections to historical docs.
- Must enforce the durable-path invariant from §5: any authoritative reference to former `tmp/unknown_link_*` docs must use durable `docs/RE/` paths.

**Acceptance criteria:**
- Consolidated parity matrix passes for all five component types × all operation modes.
- Residual-gap catalog produced; every required implementation gap is resolved through a separate slice before closure; explicitly optional gaps documented with rationale.
- Documentation closed: no stale parity statements, dated corrections prepended where history is preserved.
- Final runtime gate: Strategy executes in Desktop without reverify; other types verified to the extent feasible.
- No source-text parsing, no password fiction, no scope creep.

**Mandatory CLI E2E:**
- Full real CLI E2E per `docs/E2E_TEST_SPEC.md` for all five component types and all operation modes. This is the closure gate. Must pass before M.5 is considered done.

**Review/handoff requirements:**
- Adversarial review: confirm matrix completeness, documentation closure correctness, no silent history rewriting, no scope creep, complete execution.
- Handoff note recording residual gaps (if any) and follow-up slices opened.

---

## 3. Scope contamination prevention (hard rules for all M slices)

1. **Not the confirmed immediate root cause.** No M item may be presented, scoped, tested, or accepted as a root-cause fix. The root cause is Workstream L. Any conflation is a DISAPPROVE finding.
2. **No source-text metadata parsing.** Names/types/ProgIds come from orlib20 warehouse records or APIs. Applies explicitly to M.4 contract-change detection.
3. **No password-protection fiction.** No user-facing password concept, CLI flag, Python parameter, or error type.
4. **No bypass restoration.** No restoration/re-addition/emulation of the removed `FUN_10011230` bypass.
5. **No cross-slice bundling.** Each M.x is a separate TVS. One subagent → one narrowly scoped subtask.
6. **No TradeStation installation modification.** Read-only.
7. **Do not reopen or hold L.** M retains its independent statuses and blockers.

---

## 4. Safe recursion/cycle handling and failure reporting for dependent reverification (M.4-specific)

**Required safety properties (any M.4 implementation must satisfy these):**
- **Repeated-node/cycle safety:** The traversal must not hang, stack-overflow, or infinitely recurse. Prove with a synthetic test graph first. Use a live warehouse cycle only if supported construction is confirmed.
- **Depth bound:** Match a confirmed TSDev bound when one exists; otherwise establish a reviewed finite-safety invariant.
- **Failure isolation:** A per-dependent reverify failure must not corrupt the root operation's durable result nor leave partial warehouse state. Failure reporting must include the dependent's name and ProgId (warehouse-derived).
- **Failure reporting shape:** Per-dependent failures surfaced in CLI/Python result structure with enough identity detail to locate the failing dependent. Exact shape delegated to M.4's TDD plan and adversarial review.
- **No silent success:** If any dependent reverify fails, the operation must not report unqualified success. Either fail with dependent detail, or succeed-with-warnings per TSDev's confirmed behavior.

**Not prescribed (delegated to M.4 research):** the exact visited-set data structure, traversal order, which supported primitives implement TSDev.exe's behavior, and the exact contract-change detection field.

---

## 5. Version/signature impact decision gates and documentation updates

**Gate conditions (before merge of any M slice with resolver/catalog impact):**
- Classify every new target: named/ordinal PE exports use export-resolution metadata; non-exported internal functions use byte signatures. Do not represent an export as a byte signature.
- If M.1 confirms ordinals 66/67, add named/ordinal export resolver support, update manifest export list and `EXPECTED_EXPORT_COUNT`, regenerate via build, update catalog/resolver tests. If non-exported, use signature path and update `EXPECTED_SIGNATURE_COUNT`.
- Apply same classification to M.2 CELProperties targets and M.4 orlib20 primitives. Do not add a signature for TSDev.exe `FUN_004D91E0`; M.4 replicates its behavior without coupling the CLI to TSDev.exe.
- Update each changed expected count in its matching constant; never hand-edit `signatures.gen.h`; no dummy entries.
- Version detection (Tier 1/2/3) must remain unchanged. Run all mismatch tests after any resolver/index cleanup.

**Documentation updates required per slice:**
- M.1: update `docs/SIGNATURE_CATALOG.md` if ordinals 66/67 added; update `src/tsdev_lib.cpp` comments; slice-local findings note.
- M.2: update save-path comments; document encryption branch if confirmed; slice-local findings note.
- M.3: document `0x40A` format/packing/lifetime in a durable note; update `docs/E2E_TEST_SPEC.md` if receiver-aware live check added.
- M.4: document traversal, contract-change detection, cycle handling, failure reporting in a durable note.
- M.5: close documentation gaps across `docs/SIGNATURE_CATALOG.md`, `docs/FEASIBILITY.md`, `docs/PROJECT_HISTORY.md`, `docs/E2E_TEST_SPEC.md`, `docs/TS_COMPONENT_TYPES.md` with dated corrections.

**Durable-path invariant:** Any authoritative reference to former `tmp/unknown_link_*` research in `docs/FEASIBILITY.md` or `docs/SIGNATURE_CATALOG.md` must cite durable `docs/RE/` paths. M.5's closure review validates this.

---

## 6. Exit criteria and risk register

### Workstream M exit criteria (all must hold)
- M.1–M.5 (including M.3b) each approved by adversarial review with no open findings.
- Each M slice's mandatory CLI E2E passed for all five component types in the relevant operation modes.
- No M slice introduced source-text parsing, user password fiction, bypass restoration, or root-cause conflation.
- Consolidated parity matrix (M.5) passes; residual gaps cataloged or opened as follow-up slices.
- Documentation closed with dated corrections where applicable.
- Final runtime gate: Strategy executes in Desktop without reverify after the full M workstream; other component types verified to the extent feasible.
- `AGENTS.md` acceptance criteria (CLI + Python wrapper reliability) still hold.

### Risk register
| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| M slice conflated with L root cause | Medium | High | §3 hard rules; adversarial review checks for conflation; DISAPPROVE on any conflation. |
| M.4b.1 dependency DB not populated by identified mechanism | Medium | High (M.4b.2 blocked) | Session 3 identified 13 missing TSDev commands; Ghidra analysis is the next step. |
| M.4 cycle termination not established when `contractChanged=true` | Medium | High | Separate M.4 blocker; M.4b.2 resolves old-record capture/gate + synthetic/runtime cycle coverage. |
| M.2 reintroduces user password fiction | Low | High | §3 hard rule; adversarial review; no user-facing password surface. |
| M.4b warehouse-level approach requires orlib20 internal coupling | Medium | Medium | CLI already drives cmd 0x0D for `exists`; M.4b uses only confirmed, already-used mechanisms. |
| M.5 documentation closure silently rewrites history | Low | Medium | §5 requires dated corrections prepended; adversarial review checks. |
| M slice expands scope into unrelated orlib20 subsystems | Medium | Medium | §3 hard rule; one subagent → one narrowly scoped subtask; adversarial review. |
| M.4 blocker overlooked → M.5 starts prematurely | Medium | High | No M.5 closure claim until M.4b dependency gate is resolved. |
| Signature/catalog count drift after M.1/M.2/M.4 | Medium | Medium | §5 gate; regenerate via build, not hand-edit; run all mismatch tests. |

---

## 7. Atomic subagent orchestration and adversarial review requirements

**Orchestration (per `AGENTS.md`):**
- Each M.x is one TVS owned by the future PM, but no single subagent executes an entire TVS. Decompose into atomic research, RED-test edit, RED verification, implementation edit, build, GREEN verification, regression, documentation, review, and real-CLI E2E subtasks.
- Each R-M.x.y question is a separate narrow subtask unless two are inseparable. Use the `universal` profile by default; fall back to `subagent_general` only if `universal` fails. Use `subagent_explore` only for trivial read-only lookups.
- If research reveals additional independent concerns, add reviewed subtasks or propose a new TVS; never expand one subagent's scope silently.
- Order: M.1 → M.2/M.3 (parallel after M.1) → M.3b.2c → M.4a → M.4b.1 → M.4b.2 → M.5. The executing PM re-confirms dependencies and highest ROI before each slice.
- Each slice updates `docs/PM_WORKING_MEMORY.md` with its state only as part of that slice's own work.
- Any slice adding/registering tests updates CMake/runner wiring and all live current-count summaries from actual collected definitions in that same slice; do not defer count accuracy to M.5.

**Adversarial review (per `AGENTS.md`):**
- Every M slice result must be adversarially reviewed by an independent subagent.
- The reviewer receives the original slice instructions and the result. It checks for:
  - Complete execution of the slice instructions (objective 100% met).
  - No scope creep (no cross-slice bundling, no unrelated orlib20 subsystems).
  - No drift (no root-cause conflation, no presenting parity as the fix).
  - No undue shortcuts/hacks (no source-text parsing, no password fiction, no bypass restoration, no suppressed compiler errors).
  - TDD red→green actually followed (red test committed and shown failing before green).
  - Mandatory CLI E2E actually run and passed for all five component types in the relevant modes.
  - Documentation/signature gate (§5) satisfied if the slice touches signatures/resolver/catalog.
- On DISAPPROVE: an implementer subagent fixes the findings, then another adversarial reviewer reviews again. Loop until APPROVE.
