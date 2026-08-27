# Workstream M — TSDev Verify/Library Parity Follow-ups

**Status:** M.1/M.2/M.3 are approved historical slices; M.3b is planned;
M.4 implementation exists but is manual/blocked by the CLI-created object-code
defect and separate pre-existing review escalations; M.5 is pending. This is the active M
plan; historical slice results are not
current production-acceptance evidence.
**Review status:** Independent review loops APPROVED; final hands-on adversarial PM review APPROVED on 2026-08-23 after correcting evidence-gate ownership, receiver scope, encrypted-fixture safety, cycle testing, TSDev.exe coupling, resolver target classification, closure gating, and atomic subagent policy. This historical plan review does not establish current acceptance.
**Priority:** Follow-up parity work after Workstream L. M.3b is the next planned slice because its CLI-created object-code defect blocks the M.4 dependency test; M.5 follows M.4.
**Dependency on L:** The Workstream L dependency is satisfied by the current Workstream L status above. M.1–M.3 remain approved historical slices; M.3b remains planned, M.4 remains manual/blocked by the CLI-created object-code defect and separate pre-existing review escalations, and M.5 remains pending.
**Source authority (approved findings only):**
- `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md` — root cause, production fix design, and §L follow-up parity section (durable promotion of the former `tmp/unknown_link_root_cause_and_plan.md`).
- `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md` — durable static appendix (call chains, ABI, COM ordering, loader GUID check; durable promotion of the former `tmp/unknown_link_ghidra_evidence.md`).
- `docs/PM_WORKING_MEMORY.md` §§2–4 — current workstream status, gates, and audit ledger.
- `AGENTS.md` — emulate-TSDev rule, no-hacks rule, no-source-text-metadata rule, TDD/TVS/adversarial-review rules, mandatory CLI E2E rule.
- `docs/TS_COMPONENT_TYPES.md` — confirmed warehouse type mappings for the five supported component types.

**Durable-evidence prerequisite (historical gate before M started) — SATISFIED:**
- During the 2026-08-23 planning/documentation foundation, the approved
  `tmp/` research artifacts were promoted to tracked durable storage under
  `docs/RE/` (`RE_UNKNOWN_LINK_ROOT_CAUSE.md` and
  `RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`). Promotion preserved substantive
  approved technical content with status/relationship headers,
  transient-artifact annotations, and path/anchor adjustments; conclusions
  and scope are unchanged. **This evidence gate is satisfied independently of
  Workstream L implementation status.**
- This historical evidence gate is satisfied. The current Workstream L status is
  stated in the document header. Neither fact establishes current M acceptance;
  the current M slice statuses and blockers remain as stated above.
- M slices must cite the durable `docs/RE/` paths, not former
  `tmp/unknown_link_*` document paths, in handoff notes and findings.
- Raw experiment artifacts that remain under `tmp/` (Frida traces, probe
  stdout, ELL/XML/GUID analysis files) are transient/reproducible raw evidence,
  not authoritative research documents. Future teams must reproduce any raw
  evidence they need if those files are unavailable.

## 1. Goals, non-goals, confirmed facts, open questions

### Goals
- Bring CLI/library behavior closer to TSDev.exe emulation for the four parity concerns already enumerated in §L of the root-cause plan (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`), plus a closure slice that consolidates verification and documentation across all five supported component types and operation modes.
- Each M.1–M.5 is an independent thin vertical slice (TVS): one narrowly scoped concern, TDD red→green, mandatory real CLI E2E, adversarial review, handoff.
- Future teams executing a slice are explicitly allowed and expected to perform the slice-scoped research (Ghidra/Frida) needed to answer that slice's open questions. That research is bounded to the slice and must not expand its boundary.

### Non-goals (hard)
- M is **not** the confirmed immediate root cause. The immediate root cause (bypassed `FUN_10011230` finalization under missing STA COM) is Workstream L and is already confirmed. No M item may be presented, scoped, or accepted as a root-cause fix.
- No M item may reintroduce source-text metadata parsing. Per `AGENTS.md` ("Never rely on mutable item internals for correctness"), names/types/ProgIds come from orlib20 warehouse records or APIs, never from item source content. Any slice that appears to need source parsing is out of scope and must be re-scoped to a warehouse/API source.
- No M item may invent user-facing password protection. Per the corrected findings in `docs/RE/RE_M2_SAVEMODULE_NEW.md` §6, there is no user password protection for library items; TSDev may internally encrypt on disk, hidden from the user, and the user always sees plain text. M.2 deals with internal source encryption semantics only and must not fabricate a user password feature.
- No M item may restore, conditionally re-add, or emulate the removed `FUN_10011230` bypass, nor add compensating stack manipulation for the removed patch.
- No new reverse engineering of unrelated orlib20 subsystems. Slice-scoped RE is permitted only where explicitly delegated below.
- No modification of the TradeStation installation directory (read-only per `AGENTS.md`).

### Confirmed facts (baseline for all M slices)
- TSDev UI Verify command chain (static, every direct call cited): `WM_COMMAND 0x296A → FUN_0047B0E0 → FUN_00451800 → FUN_004D9480 → FUN_004D91E0 → orlib20!_VerifyModule_New@20` (`docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md` §6).
- TSDev UI Verify flow ordering: save source (`_SaveModule_New@20` with non-NULL flags and an internal metadata/encryption buffer) → allocate/configure 0x58-byte `CELCompiler` → `StartCompilingProcess` → `_VerifyModule_New@20` → dependent-program handling → `EndCompilingProcess` → success event `0x40A` → properties/view refresh → compiler destruction (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md` §B ("Symptom and reproduction facts"); `docs/RE/RE_M3_0x40A_NOTIFICATION.md` §2.5).
- TSDev configures `CELCompiler` fields: `+0x08`=category, `+0x0C`=subtype, `+0x10`=current ProgId, `+0x18`=notification HWND; clears `+0/+4` before compilation (`docs/M1_HANDOFF.md` §R-M.1.2).
- The pre-M.1 CLI diverged: `+0x10` was hardcoded `0xA02`, `+0x08`/`+0x0C` were omitted, and 6 was written at `+0x28` (unexplained; `+0x28` is inside an empty inline `std::wstring` buffer). M.1 records the correction of these field writes (`docs/M1_HANDOFF.md`; current implementation `TsDevLib::verifyModule` in `src/tsdev_lib.cpp`).
- The former `+0x10=0xA02` divergence was **not** the root cause for the direct `_VerifyModule_New` call: `_VerifyModule_New` uses its explicit ProgId argument throughout; `+0x10` is consumed by TSDev's outer recursive wrapper, which the CLI does not call (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md` §F ("Disproved or non-causal hypotheses")).
- Post-success `0x40A` is a live-only notification for already-running clients; it is **not** the durable link fix (Desktop restart did not repair a broken CLI item) (`docs/RE/RE_M3_0x40A_NOTIFICATION.md` §6; root-cause plan (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`) §F ("Disproved or non-causal hypotheses")).
- Reverse-dependent revalidation (TSDev `FUN_004D91E0` traversal) propagates signature/contract changes to reverse dependents/callers; it is **not** required to make the root Strategy itself linkable (`docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md` §1.2; root-cause plan (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`) §F ("Disproved or non-causal hypotheses")).
- The non-NULL password-shaped/internal metadata argument in `_SaveModule_New` controls source encryption/protection behavior, not final linking; it is not a user password feature (`docs/RE/RE_M2_SAVEMODULE_NEW.md` §4; root-cause plan (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`) §F ("Disproved or non-causal hypotheses")).
- Runtime loader uses `.ell` (ELRte/ELMDCore), with expected-GUID vs loaded-ELL-GUID validation and null return on mismatch; XML is debugger/source-map only (`docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md` §4).
- Five supported component types and their warehouse mappings: Strategy (whType 0), Indicator (whType 2), Function (whType 4), TradingApp (whType 5122 = `(subType 0x14 << 8) | category 0x02`), ShowMe (whType 1282 = `(subType 0x05 << 8) | category 0x02`). whType is packed as `(subType << 8) | category` for TradingApp/ShowMe (and any other composite type); API functions take category (low 8 bits); `.ell` filenames use expanded whType (`docs/TS_COMPONENT_TYPES.md`).

### Open implementation questions (delegated per slice, not pre-answered here)
Each is listed under its owning slice below. None is answered in this document; each future TVS team owns its slice's research.

---

## 2. Slice specifications (M.1–M.5)

Each slice follows the same structure: objective, known factual baseline, research questions delegated to that future TVS, implementation boundary, likely files/systems, TDD plan, acceptance criteria, mandatory CLI E2E, review/handoff requirements.

### M.1 — Exact CELCompiler allocation/setup/destruction parity

**Current status:** APPROVED historical slice. The pre-implementation baseline
and acceptance text in this subsection describe the M.1 plan and are not a
current acceptance result; see `docs/M1_HANDOFF.md` for the retained slice
record.

**Objective:** Make CLI `CELCompiler` allocation, field setup, and destruction match TSDev's UI Verify path exactly, so that the compiler object presented to `_VerifyModule_New` is byte-for-byte equivalent in the fields TSDev sets.

**Known factual baseline (approved findings only):**
- TSDev allocates a 0x58-byte `CELCompiler` and sets `+0x08`=category, `+0x0C`=subtype, `+0x10`=current ProgId, `+0x18`=notification HWND; clears `+0/+4` before compilation (`docs/M1_HANDOFF.md` §R-M.1.2).
- The pre-M.1 CLI hardcoded `+0x10=0xA02`, omitted `+0x08`/`+0x0C`, and wrote 6 at `+0x28` (historical baseline recorded in `docs/M1_HANDOFF.md`); M.1 records the correction.
- The former `+0x28` write was unexplained; `+0x28` is uninitialized heap and must not be relied upon; M.1 removed the write (`docs/M1_HANDOFF.md` §R-M.1.3).
- The former `+0x10=0xA02` divergence was not causal for the direct call (root-cause plan (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`) §F ("Disproved or non-causal hypotheses")); M.1 corrected it as a parity change, not as a root-cause fix.
- Root-cause plan §L ("Historical follow-up TSDev parity work") references "ordinal 66/67 factory/destruction path" for TSDev's compiler allocation/destruction.

**Research questions delegated to this future TVS (slice-scoped only):**
- R-M.1.1: Confirm the exact ordinal(s) TSDev uses for `CELCompiler` allocation and destruction. Is it ordinal 66 and 67 of orlib20, or a confirmed equivalent export pair? Cite Ghidra evidence (export table + xrefs from the UI Verify chain).
- R-M.1.2: Confirm the exact byte layout of the 0x58-byte `CELCompiler` object at `+0x00`, `+0x04`, `+0x08`, `+0x0C`, `+0x10`, `+0x18`, `+0x28` as observed in the TSDev UI Verify path (Frida read of the object immediately before `_VerifyModule_New`).
- R-M.1.3: Determine whether TSDev writes anything to `+0x28`. If not, the CLI's write of 6 must be removed. If TSDev writes a non-zero value, document the exact value and condition.
- R-M.1.4: Determine the notification HWND semantics at `+0x18`: does TSDev pass a real window handle, NULL, or a sentinel? Does the CLI need a hidden message-only window, or is NULL acceptable for headless operation? Cite evidence. Reconcile the compiler `+0x18` HWND with (a) the CLI's current `m_editorHwnd` member and (b) the editor HWND passed to `_SaveModule_New`. Separately determine whether M.3's `0x40A` sender requires any CLI-owned window at all; do not assume the sender and registered external receivers share M.1's editor/notification window. Coordinate test-only receiver machinery with R-M.3.4, but create no shared production window abstraction unless evidence requires it. The handoff must provide one evidence-backed ownership/lifetime story for each distinct handle.
- R-M.1.5: Confirm category/subtype source: are they derived from the warehouse type mapping in `docs/TS_COMPONENT_TYPES.md` (expanded whType decomposition), or from a separate TSDev-side lookup? Ensure no source-text parsing.

**Implementation boundary:**
- In scope: `CELCompiler` allocation, field setup (`+0x00`, `+0x04`, `+0x08`, `+0x0C`, `+0x10`, `+0x18`), `+0x28` write removal/justification, destruction via the confirmed ordinal/equivalent.
- Out of scope: changing `_VerifyModule_New` arguments, SaveModule flags (M.2), `0x40A` (M.3), dependent revalidation (M.4), any root-cause-related change, any source-text parsing, any user password feature.
- Must not alter the explicit ProgId argument to `_VerifyModule_New` (already correct).

**Likely files/systems:**
- `src/tsdev_lib.cpp` (`TsDevLib::verifyModule`, current compiler allocation,
  TSDev-parity field writes, verification, and destruction flow).
- `src/tsdev_lib.h` (compiler member declarations).
- `src/orlib_resolver.h` / `src/orlib_resolver.cpp` (ordinal resolution if ordinals 66/67 are confirmed).
- `signatures/*.json`, resolver code, and `docs/SIGNATURE_CATALOG.md` only if M.1 adds resolution metadata. Classify each target correctly: named/ordinal exports belong in export-resolution metadata and export counts; only non-exported internal targets use byte signatures (coordinate with §5).
- Tests: `tests/test_compiler_layout.cpp` (or another new focused
  compiler-setup test), E2E in `docs/E2E_TEST_SPEC.md`.

**TDD plan (red → green):**
1. Historical RED baseline: the unit/introspection test constructed `TsDevLib`
   and exposed the pre-M.1 field mismatches (`+0x10=0xA02`, missing
   category/subtype, `+0x28=6`). The current M.1 implementation writes the
   TSDev-parity fields.
2. Research R-M.1.1–R-M.1.5 within this slice; record evidence in a slice-local findings note (e.g. `tmp/m1_*.md`).
3. GREEN: Implement exact field setup using warehouse-derived category/subtype (from `docs/TS_COMPONENT_TYPES.md` mapping) and the confirmed ProgId; remove the `+0x28` write unless R-M.1.3 proves TSDev writes there; use the confirmed allocation/destruction ordinal or equivalent.
4. Add regression assertions that the compiler object is destroyed exactly once per operation and that no field is left stale across batch/serve reuse.

**Acceptance criteria:**
- `CELCompiler` field layout matches TSDev evidence for all five supported component types.
- `+0x28` write is removed or justified by cited TSDev evidence.
- Allocation and destruction use the confirmed ordinal/equivalent; no leak, no double-destroy across single/batch/serve.
- No source-text parsing introduced. No user password feature introduced.
- All existing regression suites pass; new compiler-setup tests pass.

**Mandatory CLI E2E:**
- Run the real CLI E2E per `docs/E2E_TEST_SPEC.md` for all five component types (Strategy, Indicator, Function, TradingApp, ShowMe) across insert/modify/upsert. Confirm successful compile/verify, fresh ELL/XML pair, source round-trip, and (for Strategy) Desktop execution without reverify. The E2E must pass before M.1 is considered done.
- **Operation-mode coverage (do not defer to M.5):** Because M.1 changes compiler allocation/setup/destruction — which is reused across batch and serve sessions — the M.1 E2E gate must additionally cover, for at least one representative component type (Strategy): (a) manifest/stdin batch mode (multiple operations in one CLI process, asserting no compiler leak/double-destroy and no stale fields across the batch), and (b) serve mode (a single serve session handling multiple operations, asserting the same). Full per-type × per-mode matrix coverage remains M.5's closure gate; M.1 only proves the compiler-setup change is safe under reuse for one type in batch and serve.

**Review/handoff requirements:**
- Adversarial review by an independent subagent: verify no scope creep into M.2/M.3/M.4, no root-cause conflation, no source-text parsing, no password fiction, complete execution of slice instructions.
- Handoff note recording confirmed ordinals, field layout, and any signature/catalog impact (feed into §5 gate).

---

### M.2 — `_SaveModule_New` flags and CELProperties before/after verify parity

**Current status:** APPROVED historical slice. The pre-implementation baseline
and acceptance text in this subsection describe the M.2 plan and are not a
current acceptance result; see `docs/M2_HANDOFF.md` for the retained slice
record.

**Objective:** Reproduce TSDev's `_SaveModule_New` flags and CELProperties metadata before and after verify, including safe handling of internal source encryption semantics — without inventing user password protection.

**Known factual baseline (approved findings only):**
- TSDev UI Verify saves before compiling: `_SaveModule_New@20` enters/returns success, then `StartCompilingProcess`, then `_VerifyModule_New@20` (`docs/RE/RE_M2_SAVEMODULE_NEW.md` §2; root-cause plan (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`) §B ("Symptom and reproduction facts")).
- `_SaveModule_New@20` is called with non-NULL flags/password pointers in the TSDev UI path (`docs/RE/RE_M2_SAVEMODULE_NEW.md` §2; root-cause plan (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`) §L ("Historical follow-up TSDev parity work")).
- Non-NULL password controls source encryption/protection behavior, not final linking (`docs/RE/RE_M2_SAVEMODULE_NEW.md` §4; root-cause plan (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`) §F ("Disproved or non-causal hypotheses")).
- There is no user password protection for library items; TSDev may internally encrypt on disk, hidden from the user; the user always sees plain text (`docs/RE/RE_M2_SAVEMODULE_NEW.md` §6).
- `getSource()` already handles internal encryption: when the 0x400 flag is set, it passes a zeroed 64-byte buffer to trigger Branch C decryption and returns plain text (`docs/RE/RE_M2_SAVEMODULE_NEW.md` §4d). M.2 is concerned with the save-side parity, not the already-fixed get-side.

**Research questions delegated to this future TVS (slice-scoped only):**
- R-M.2.1: Confirm the exact `_SaveModule_New@20` flag values TSDev passes before verify (the "non-NULL flags/password pointers" observed). Decompose the flags bitfield against Ghidra evidence.
- R-M.2.2: Confirm the exact CELProperties metadata TSDev sets before/after verify. What fields, what order, what values per component type?
- R-M.2.3: Determine the save-side encryption semantics: does TSDev pass a non-NULL zeroed password buffer (mirroring the get-side Branch C pattern) to trigger internal encryption on save for items that should be encrypted? Or is encryption controlled entirely by the 0x400 flag/master record? Cite Ghidra evidence for the save path.
- R-M.2.4: Determine whether the pre-verify save and any post-verify save differ in flags/properties, and whether the CLI must perform both.
- R-M.2.5: Confirm that no user-facing password parameter is needed (consistent with the corrected findings). If any password-shaped buffer is passed, it must be an internal zeroed buffer mirroring TSDev, never a user-supplied secret.

**Implementation boundary:**
- In scope: `_SaveModule_New` flag values, CELProperties metadata setup before/after verify, internal encryption save-side parity (zeroed internal buffer if confirmed), save ordering relative to compile/verify.
- Out of scope: any user password UI/CLI flag, any user-supplied secret, source-text parsing, compiler object setup (M.1), `0x40A` (M.3), dependent revalidation (M.4), root-cause changes.
- Must not introduce a `PasswordProtectedError` or any user-facing password concept (those were removed in the get-source correction).

**Likely files/systems:**
- `src/tsdev_lib.cpp` (saveSource / save path; verifyModule ordering).
- `src/tsdev_lib.h` (save/properties members).
- `src/orlib_resolver.h` / `src/orlib_resolver.cpp` (`_SaveModule_New` and CELProperties export resolution).
- Tests: `tests/test_save_module.cpp`, `tests/test_get_source.cpp`, and E2E.

**TDD plan (red → green):**
1. RED: Add a test that performs an insert/modify and asserts the save flags and CELProperties match TSDev's confirmed values (placeholder asserts that fail until R-M.2.1/R-M.2.2 answers are encoded). Add a round-trip test: source saved by CLI, read back by `getSource()`, must equal the original plain text for both encrypted-flag-set and flag-clear items.
2. Research R-M.2.1–R-M.2.5 within this slice; record evidence in a slice-local findings note.
3. GREEN: Implement the confirmed flags/properties; if internal encryption save-side parity requires a zeroed internal buffer, add it with a comment naming the TSDev branch; ensure `getSource()` still round-trips.
4. Add regression: encrypted-flag-set item saved by CLI decrypts correctly via `getSource()`; flag-clear item round-trips unchanged.

**Acceptance criteria:**
- `_SaveModule_New` flags and CELProperties match TSDev evidence for all five component types.
- Internal encryption save-side parity is implemented only if R-M.2.3 confirms it; otherwise documented as not required.
- No user-facing password concept exists in CLI, Python wrapper, or docs after this slice.
- Source round-trips byte-for-byte for both encrypted-flag-set and flag-clear items across insert/modify/upsert.
- No source-text parsing introduced.

**Mandatory CLI E2E:**
- Run real CLI E2E per `docs/E2E_TEST_SPEC.md` for all five component types and require PASS.
- **Known encrypted-source fixtures are read-only baselines only:** `BTA_PROTEGE-SmartCode-JW3` and `Average` may be used for non-destructive `get-source` confirmation. Never insert/modify/delete/upsert these built-ins, and do not claim that copying their plaintext source automatically reproduces encrypted warehouse metadata.
- R-M.2.3 must identify a TSDev/orlib-supported way to create or prepare a disposable encrypted test item without mutating built-ins or editing warehouse files directly. Use that disposable item for save/verify/get-source round-trip and cleanup. If no safe supported fixture can be created, the encrypted-save acceptance cell remains blocked or is verified at a lower test boundary with an explicit limitation; do not improvise metadata mutation or weaken the assertion silently.
- **Operation-mode coverage (do not defer to M.5):** Because M.2 changes `_SaveModule_New` flags/CELProperties on every mutation, cover at least Strategy in (a) manifest/stdin batch mode and (b) one reused serve session. Assert flags/properties and flag-clear source round-trip on every iteration. Exercise encrypted save-side behavior in these modes only after the safe disposable fixture mechanism is confirmed. Full per-type × per-mode coverage remains M.5's closure gate.

**Review/handoff requirements:**
- Adversarial review: confirm no user password fiction, no source-text parsing, no scope creep into M.1/M.3/M.4, no root-cause conflation, complete execution.
- Handoff note recording confirmed flags/properties and encryption save-side decision (feed into §5 gate if signature impact).

---

### M.3 — Post-success `0x40A` library-change notification for already-running clients

**Current status:** APPROVED historical slice. The baseline and acceptance text
in this subsection describe the M.3 plan and are not a current acceptance
result; see `docs/M3_HANDOFF.md` for the retained slice record.

**Objective:** Reproduce TSDev's post-success `0x40A` library-change notification so already-running registered TradeStation consumers refresh affected in-memory module/runtime state after a CLI operation that TSDev confirms sends `0x40A`, with correct category/ProgId packing, lifetime/error semantics, and receiver-aware tests. Do not assume TSDev.exe itself is a receiver; R-M.3.1 must identify the actual registered receiver set. The exact set of sending operations (insert, modify, delete, upsert, and/or others) is **not** precommitted here; R-M.3.2 confirms it from TSDev behavior. The CLI must neither send for an unconfirmed operation nor omit a confirmed send.

**Known factual baseline (approved findings only):**
- TSDev UI Verify flow includes success event `0x40A` after `EndCompilingProcess` (`docs/RE/RE_M3_0x40A_NOTIFICATION.md` §2.5).
- `0x40A` is a live-only notification for already-running consumers; it is **not** the durable link fix (Desktop restart did not repair a broken CLI item) (`docs/RE/RE_M3_0x40A_NOTIFICATION.md` §6; root-cause plan (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`) §F ("Disproved or non-causal hypotheses")).
- The pre-M.3 CLI had only "partial equivalents" for post-success parity (root-cause plan (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`) §D table row "Post-success parity"); this is a historical baseline.

**Research questions delegated to this future TVS (slice-scoped only):**
- R-M.3.1: Confirm the exact `0x40A` message format: which window/message target receives it (broadcast, specific TSDev window class, registered window message), and how category and ProgId are packed into wParam/lParam.
- R-M.3.2: Confirm the exact set of operations for which TSDev sends `0x40A` (insert/modify/delete/upsert and any others), and the lifetime semantics for each: is `0x40A` sent only after successful verify, or also after successful delete? After failed compile? After a no-op (e.g., modify with identical source)? Cite TSDev UI evidence per operation. This R-M.3.2 answer is authoritative for M.3's operation scope; the implementation must send `0x40A` for exactly the confirmed operation set and no others. Do not precommit delete/upsert behavior in this plan.
- R-M.3.3: Confirm error semantics: what does TSDev do if `0x40A` send fails (no receiver, bad window handle)? Does it fail the operation, log, or ignore?
- R-M.3.4: Determine receiver-awareness for testing: can a test fixture register a receiver window and assert it gets the notification with correct packing? Coordinate with the hidden-window requirement possibly introduced in M.1 (R-M.1.4).

**Implementation boundary:**
- In scope: sending `0x40A` (or confirmed equivalent) with correct category/ProgId packing after exactly the operation set confirmed by R-M.3.2 (no more, no fewer); receiver-aware test fixture; lifetime/error handling matching TSDev per operation.
- Out of scope: durable link fix (L), compiler setup (M.1), save flags (M.2), dependent revalidation (M.4), source-text parsing, user password.
- Must not make `0x40A` a substitute for durable linking or block on receiver presence.
- Must not precommit delete/upsert (or any other operation's) `0x40A` behavior ahead of R-M.3.2; if R-M.3.2 is inconclusive for a given operation, that operation does not get `0x40A` and the gap is recorded in the handoff note (do not guess).

**Likely files/systems:**
- `src/tsdev_lib.cpp` (post-success path after verify/delete).
- `src/tsdev_lib.h` (notification member).
- Test-only receiver window/message helper. Add production window machinery only if R-M.3.1 proves the sender itself requires it; do not conflate sender state with external receiver registration.
- Tests: a receiver-aware fixture (test-only window proc), E2E.

**TDD plan (red → green):**
1. Historical RED baseline: the receiver-aware test registered a test window,
   performed an operation from the R-M.3.2 set, and initially failed because
   the pre-M.3 code emitted no `0x40A`. The current M.3 implementation sends
   the notification with the confirmed category/ProgId packing; the negative
   cases remain part of the regression contract.
2. Research R-M.3.1–R-M.3.4 within this slice; record evidence in a slice-local findings note. R-M.3.2's confirmed operation set is the authoritative scope for both positive and negative tests.
3. GREEN: Implement `0x40A` send with confirmed packing and lifetime/error semantics for exactly the R-M.3.2-confirmed operation set. Ensure send failure does not fail a successful durable operation (unless TSDev does otherwise, per R-M.3.3).
4. Add regression: no receiver present → operation still succeeds; receiver present → notification received with correct packing for all five component types, for each operation in the confirmed set; operations outside the confirmed set emit no `0x40A`.

**Acceptance criteria:**
- `0x40A` sent after exactly the R-M.3.2-confirmed operation set, with correct category/ProgId packing for all five component types; not sent for operations outside that set.
- Lifetime matches TSDev (per R-M.3.2, per operation); error handling matches TSDev (per R-M.3.3).
- Operation success is not blocked by receiver absence.
- No source-text parsing, no password fiction, no scope creep, no precommitted delete/upsert behavior ahead of R-M.3.2.

**Mandatory CLI E2E:**
- Run real CLI E2E per `docs/E2E_TEST_SPEC.md` for all five component types and require PASS.
- The automated parity gate is a registered receiver fixture that observes one correctly packed `0x40A` for every operation in the R-M.3.2-confirmed set, no notification outside that set, and successful operations when no receiver exists. Do not use TSDev's library view as proof unless R-M.3.1 independently confirms TSDev.exe is a registered receiver.
- Where the confirmed receiver is a Desktop runtime process, add a controlled human observation (or approved non-UI receiver instrumentation) that an already-applied affected study/runtime refreshes without restart. Project agents must not operate TradeStation Desktop through computer-use; record user/manual evidence when UI validation is required.
- **Operation-mode coverage (do not defer to M.5):** For at least Strategy and one R-M.3.2-confirmed operation, cover (a) manifest/stdin batch and (b) a reused serve session. Assert exactly one notification per operation, with no missed/duplicated notifications. Full per-type × per-mode coverage remains M.5's closure gate.

**Review/handoff requirements:**
- Adversarial review: confirm `0x40A` is not presented as root cause, no scope creep, correct packing, receiver-aware test rigor, complete execution.
- Handoff note recording confirmed message format, packing, lifetime/error semantics.

---

### M.3b — Fix CLI object-code version defect (version=0 blocking linker element-array population)

> **Added 2026-08-26 by PM directive.** This slice was not in the original plan. It was inserted after M.4 investigation discovered that M.4's linker-based reverse-dependent enumeration cannot work for CLI-compiled items because CLI-compiled items have object-code version=0 (instead of the correct per-category version), which causes the linker's populators to return NULL, leaving element arrays empty. M.4's implementation exists and its read-only existing-record diagnostic is retained as historical implementation evidence, but the M.4 dependency test is blocked for CLI-created items. M.4 is not an acceptance result and must not be described as green until this defect is resolved.

**Objective:** Fix the CLI compilation path so that CLI-compiled items produce object code with the correct version field (matching TSDev-created items), enabling the linker to populate element arrays and dep-lists. This unblocks M.4's reverse-dependent enumeration for CLI-compiled items.

**Known factual baseline (approved findings from M.4 investigation, all Ghidra/Frida/runtime-confirmed):**
- The linker (`FUN_101a3340`, ordinal 85) calls three populators (`FUN_101a25a0`, `FUN_101a2430`, `FUN_101a22c0`) for every item.
- The populators read the decompressed object-code version at `buffer+0x24` to locate and size element/dependency arrays.
- CLI-compiled items: `buffer+0x24 = 0` (version=0, size fields +0x18/+0x20/+0x28 also zero) → populators return NULL → element arrays empty.
- Existing TSDev-created items: `buffer+0x24 = 512 (0x200)` for Strategies (subtype 2/3), `= 4` for Functions, etc. → populators succeed → element arrays populated.
- `FUN_101d4280(category, subtype)` computes the expected version: category 0 (Strategy) returns 0x200 only when subtype is 2 or 3; category 4 (Function) always returns 4; category 2 (Indicator) is subtype-dependent.
- All sampled items (CLI-created and existing) have `master_record+0xe9 = 0` (subtype byte), so the subtype is NOT the discriminating factor for the version=0 issue.
- The .ell files are structurally identical between CLI-created and TSDev-created items (same RNAM header, same metadata tables), but the compiled object code WITHIN the .ell has version=0 for CLI-compiled items.
- The object-code version is written during compilation (`_VerifyModule_New@20` / `CELBasicCompiler::CompileText` / the WHServer COM worker), NOT during linking.
- M.1 (CELCompiler allocation/setup parity) is complete — the compiler object fields match TSDev. The version=0 defect persists despite M.1, so the cause is elsewhere in the compilation path.
- Workstream L is closed because its team completed it and a final independent review was completed. The version=0 defect is related to but distinct from the Workstream L stale-ELL issue; Workstream L closure did not fix the version=0 defect.

**Research questions delegated to this future TVS (slice-scoped only):**
- R-M.3b.1: What writes the object-code version field at `buffer+0x24`? Trace the compilation path from `_VerifyModule_New@20` through `CELBasicCompiler::CompileText` to the WHServer COM worker to find where the version is set. Cite Ghidra evidence (function addresses, field offsets, write instructions).
- R-M.3b.2: Why does the CLI compilation path produce version=0 while TSDev produces the correct version? Is there a missing call, a wrong argument, a missing field setup, or a different code path? Compare the CLI's compilation path with TSDev's using Frida tracing (read-only, no UI interaction) and/or Ghidra static analysis.
- R-M.3b.3: Does the version field depend on the CELCompiler object fields (beyond what M.1 already fixed)? Are there additional compiler fields that TSDev sets but the CLI doesn't? Check `+0x28` and any other fields not covered by M.1.
- R-M.3b.4: Does the version field depend on the `_SaveModule_New@20` flags or CELProperties metadata (M.2's scope)? Could a missing flag or property cause the compiler to produce version=0?
- R-M.3b.5: Does the version field depend on the subtype passed to `_CreateEntryEX@24`? The CLI currently passes subtype=0 for Strategies/Functions. TSDev may pass a different subtype. Check if changing the subtype affects the version field.
- R-M.3b.6: Is there a post-compilation step that TSDev performs but the CLI doesn't? For example, a "finalize" or "link" step that sets the version field after `CompileText` returns.
- R-M.3b.7: Can we fix the version field directly in the object code buffer after compilation but before linking? Or must the fix be in the compilation path itself? What are the implications of each approach?

**Implementation boundary:**
- In scope: Investigating and fixing the CLI compilation path so that CLI-compiled items produce object code with the correct version field. This may involve changes to compiler setup, save flags, subtype handling, post-compilation steps, or other compilation-path code.
- Out of scope: M.4 reverse-dependent traversal (already implemented, blocked by this defect). M.5 consolidation. Source-text parsing. User password. Bypass restoration. Workstream L recovery work.
- Must not modify orlib20.dll or any TradeStation installation file.
- Must not introduce source-text-based metadata.
- Must not suppress warnings or mask the real issue.

**Likely files/systems:**
- `src/tsdev_lib.cpp` (compilation path: `verifyModule`, `createEntry`, `saveSource`, compiler setup).
- `src/tsdev_lib.h` (compiler/save members if new fields are needed).
- `src/orlib_resolver.h` / `src/orlib_resolver.cpp` (if additional orlib20 primitives are needed).
- Tests: `tests/test_finalization_lifecycle.cpp` and
  `tests/test_reverse_dependent_revalidation.cpp`, plus new diagnostic tests
  for object-code version verification.
- Ghidra: decompilation of the compilation path (`_VerifyModule_New@20`, `CELBasicCompiler::CompileText`, WHServer COM worker).
- Frida: read-only tracing of the compilation path in both TSDev and CLI processes (no UI interaction).

**TDD plan (red → green):**
1. RED: Add a test that inserts + verifies a disposable Function via the CLI, then calls the linker (ordinal 85) on the resulting item and asserts that `record+0x29` (elemCount) > 0 and `record+0x4f` (elemArrayPtr) != NULL. This fails on current code (elemCount=0, elemArrayPtr=NULL for CLI-compiled items).
2. Research R-M.3b.1–R-M.3b.7 within this slice; record evidence in a slice-local findings note (e.g. `docs/RE/RE_M3B_OBJECT_CODE_VERSION_DEFECT.md`).
3. GREEN: Implement the fix based on the research findings. The fix must ensure CLI-compiled items produce object code with the correct version field, enabling the linker to populate element arrays.
4. Add regression: after the fix, CLI-compiled items have non-empty element arrays, dep-lists are visible, and `enumerateReverseDependents` finds reverse dependents for CLI-compiled items (not just existing TSDev-created items).

**Acceptance criteria:**
- CLI-compiled items (all five supported component types) produce object code with the correct version field (matching TSDev-created items of the same type).
- The linker (ordinal 85) populates element arrays for CLI-compiled items (elemCount > 0, elemArrayPtr != NULL).
- Dep-lists are visible at element+0x7b for CLI-compiled items that call Functions.
- `enumerateReverseDependents` finds reverse dependents for CLI-compiled items (not just existing TSDev-created items).
- The M.4 regression test (`test_reverse_dependent_revalidation.cpp`) passes for CLI-compiled items.
- No source-text parsing introduced. No user password fiction. No bypass restoration.
- All existing regression suites pass.
- No crashes, no TSCrashReport.exe.

**Mandatory CLI E2E:**
- Run real CLI E2E per `docs/E2E_TEST_SPEC.md` for all five component types. Confirm successful compile/verify AND that the linker populates element arrays for each type.
- **Object-code version verification:** For each component type, insert a disposable item, verify it, call the linker, and assert elemCount > 0 and elemArrayPtr != NULL. Clean up after.
- **Dependency graph verification:** Insert a Function, insert a caller (Strategy/Indicator) that calls the Function, modify the Function, and assert that `enumerateReverseDependents` finds the caller. Clean up after.
- **Operation-mode coverage:** Cover at least Strategy in (a) manifest/stdin batch mode and (b) serve mode. Assert element arrays are populated for every item in every operation.

**Review/handoff requirements:**
- Adversarial review: confirm the fix addresses the root cause (not a symptom), no source-text parsing, no scope creep into M.4/M.5, no bypass restoration, complete execution.
- Handoff note recording the root cause, the fix, and the evidence (feed into §5 gate if signature/catalog impact).
- Update `docs/PM_WORKING_MEMORY.md` with the fix details and unblock M.4.

**Dependency:**
- Hard dependency on M.1 (CELCompiler setup parity) — the compiler object must be correctly set up before investigating the version=0 issue.
- M.4 depends on M.3b — M.4's regression test cannot pass until M.3b fixes the version=0 defect.
- M.3b may depend on M.2 (save flags parity) if R-M.3b.4 confirms that save flags affect the version field.

---

### M.4 — Reverse-dependent revalidation equivalent to TSDev `FUN_004D91E0`

**Current status:** Implementation exists, but the CLI-created object-code
version defect leaves linker element arrays empty and blocks the dependency
test. M.4 also has separate pre-existing dependent-flag, old-master-record
capture, and cycle-safety escalations. The intended capture is missing from
`TsDevLib::saveSource()`, and `TsDevLib::verifyModule()` only consumes the
record conditionally. M.4 remains manual/blocked, has no approved post-TVS
production-E2E claim, and is not an acceptance result. The
implementation uses the linker-based enumeration path; the earlier
GetDependencies/Option B2 plan language is historical and is superseded by the
M.4 evidence note's §8.7.

**Objective:** Emulate TSDev's dependent-program traversal (`FUN_004D91E0`) so that when a Function (or other component) changes its signature/contract, its reverse dependents/callers are revalidated, with interface/contract-change detection and recursion safety.

**Known factual baseline (approved findings only):**
- TSDev UI Verify static chain includes `FUN_004D91E0` between `FUN_004D9480` and `_VerifyModule_New@20` (`docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md` §6).
- `FUN_004D91E0` is the reverse-dependent reverify wrapper; it propagates signature/contract changes to reverse dependents/callers (`docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md` §1.2; root-cause plan (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`) §F ("Disproved or non-causal hypotheses")).
- It is **not** required to make the root Strategy itself linkable (root-cause plan (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`) §F ("Disproved or non-causal hypotheses")).
- `FUN_004D91E0` reads `+0x10` of the compiler object for root bookkeeping/primary-flag selection in TSDev's outer recursive wrapper (root-cause plan (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`) §F ("Disproved or non-causal hypotheses")). The CLI does not call this TSDev-private wrapper; the implemented M.4 path replicates the traversal with the linker and warehouse records, but CLI-created object-code version `0` currently leaves the linker element arrays empty and blocks its dependency test.
- In the current CLI path, missing old-master-record capture leaves
  `contractChanged=true` by default in `TsDevLib::verifyModule()`. The
  visited-node check in `TsDevLib::traverseReverseDependents()` skips only
  when `!contractChanged`, so an already-visited node can be reverified and
  recursion can continue; this does not establish safe termination for a
  reachable cycle, without claiming that an infinite loop definitely occurs.
  The existing `tests/test_reverse_dependent_revalidation.cpp` Case 5
  ("Cycle safety (unit-level contract pin)") only pins hook signatures and
  `traversalDepth` and directly invokes synthetic hook events while deferring
  a live warehouse cycle, rather than executing runtime cycle traversal. The
  RE note's §3.2 ("No visited-set, no depth counter") and §3.5 ("No max
  depth") say static analysis cannot establish a hard cycle bound. This is a
  separate pre-existing M.4 blocker/escalation.

**Research questions delegated to this future TVS (slice-scoped only):**
- R-M.4.1: Confirm the exact traversal `FUN_004D91E0` performs: which warehouse records enumerate reverse dependents, in what order, with what per-dependent work (reverify only? recompile? both?).
- R-M.4.2: Confirm how TSDev detects an interface/contract change (signature comparison, version field, GUID, parameter list). Cite Ghidra evidence. Ensure no source-text parsing — the detection must be warehouse/metadata-derived.
- R-M.4.3: Confirm recursion/cycle handling: does `FUN_004D91E0` guard against cycles (A depends on B depends on A)? What is the visited-set mechanism? What is the max depth? Cite evidence.
- R-M.4.4: Confirm failure reporting: when a reverse-dependent reverify fails, does TSDev fail the whole operation, collect per-dependent errors, or warn? How are dependent failures surfaced to the user?
- R-M.4.5: Map the TSDev.exe wrapper's behavior onto supported orlib/warehouse primitives available to the standalone CLI. Do not load or call TSDev.exe's private `FUN_004D91E0` directly. If an equivalent exported/loadable library API is discovered, compare it with a minimal behavioral replication and choose the option that most exactly matches TSDev without coupling the CLI to TSDev.exe.

**Implementation boundary:**
- In scope: reverse-dependent traversal, contract-change detection (warehouse/metadata-derived only), recursion/cycle safety, per-dependent failure reporting.
- Out of scope: root Strategy linkability (L), compiler field setup (M.1, but may consume M.1's results), save flags (M.2), `0x40A` (M.3), source-text parsing, user password.
- Must not introduce source-text-based dependency detection. Dependencies come from warehouse records.

**Likely files/systems:**
- `src/tsdev_lib.cpp` (post-verify replication of the confirmed reverse-dependent traversal).
- `src/orlib_resolver.h` / `src/orlib_resolver.cpp` only for any additional orlib20 primitives required by the confirmed replication; never for resolving TSDev.exe code.
- Tests: a fixture with a known dependency graph (Function used by Indicator used by Strategy), E2E.

**TDD plan (red → green; historical sequence):**
1. Historical RED baseline: add a test that inserts a Function, inserts a
   caller (Indicator/Strategy) that uses it, then modifies the Function's
   signature and asserts caller revalidation. The old pre-implementation
   baseline had no traversal; the current implementation exists, but the
   CLI-created object-code version=0 defect blocks dependency discovery.
2. Research R-M.4.1–R-M.4.5 within this slice; record evidence in a slice-local findings note.
3. Historical GREEN action: implement the confirmed traversal with contract-change detection, recursion/cycle safety, and failure reporting, using TSDev's confirmed guard or an evidence-justified equivalent.
4. Required regression, not satisfied by the current Case 5: use a synthetic dependency enumerator/graph to prove repeated nodes and cycles cannot hang or stack-overflow. Add a live warehouse cycle only if R-M.4.3 confirms such a graph is valid and can be created through supported APIs; do not force invalid EasyLanguage source or direct warehouse edits. Per-dependent failures must report warehouse-derived name and ProgId, and successful acyclic revalidation must complete.

**Acceptance criteria:**
- Reverse dependents of a changed component are revalidated per TSDev's confirmed traversal.
- Contract-change detection is warehouse/metadata-derived; no source-text parsing.
- Recursion/repeated-node handling is proven safe with a synthetic graph; live-cycle behavior is tested only if supported warehouse/API construction is confirmed.
- Per-dependent failures are reported with dependent identity (name, ProgId from warehouse).
- No scope creep, no password fiction, no root-cause conflation.

**Mandatory CLI E2E:**
- Real CLI E2E per `docs/E2E_TEST_SPEC.md` plus a dependency-graph scenario: insert Function → insert caller → modify Function signature → assert caller revalidation outcome. Confirm the caller remains runtime-linkable in Desktop after successful revalidation (Strategy gate). E2E must pass before M.4 is considered done.
- **Operation-mode coverage (do not defer to M.5):** Because M.4 introduces per-operation reverse-dependent traversal state (visited-set/cycle-guard and any traversal scratch) that is created per operation and must not persist across operations, the M.4 E2E gate must additionally cover, for at least one representative reverse-dependency graph (e.g. Function → Strategy caller, with the Function modified repeatedly so its Strategy caller is revalidated each time), in both (a) manifest/stdin batch mode (multiple modify operations in one CLI process, each triggering revalidation of the same caller graph) and (b) serve mode (a single serve session handling multiple modify operations with the same caller graph, exercising serve reuse). For every operation in both modes the E2E must assert: revalidation runs for that operation (the caller is revalidated, not skipped); no visited-set or cycle-state leaks from a prior operation into a later one (a fresh traversal per operation, no false "already visited" short-circuit); and no stale traversal/revalidation state across operations (e.g. a cached "caller already verified" flag, a leftover dependency list, or a retained compiler/traversal object from a previous operation must not suppress or corrupt the current operation's revalidation). Full per-type × per-mode matrix coverage remains M.5's closure gate; M.4 only proves the reverse-dependent revalidation change is safe under repeated operations and serve reuse for one representative dependency graph in batch and serve.

**Review/handoff requirements:**
- Adversarial review: confirm recursion safety, no source-text parsing, failure reporting completeness, no scope creep, complete execution.
- Handoff note recording confirmed traversal, contract-change detection mechanism, cycle handling, and failure reporting semantics.

---

### M.5 — Consolidated parity/runtime verification and documentation closure

**Current status:** PENDING. M.5 is not a current acceptance result; it follows
resolution of the M.3b blocker and the M.4 dependency-test gate.

**Objective:** Consolidate parity and runtime verification across all five supported component types (Strategy, Indicator, Function, TradingApp, ShowMe) and all operation modes (single CLI, manifest/stdin batch, directory batch, serve single, Python batch-over-serve), and close documentation gaps opened by M.1–M.4.

**Known factual baseline (approved findings only):**
- Five supported component types and warehouse mappings are confirmed (`docs/TS_COMPONENT_TYPES.md`).
- Operation modes: single CLI, batch (manifest + stdin), directory batch, serve, Python batch-over-serve (`AGENTS.md`; `docs/workstream_serve_plan.md`).
- The root-cause plan (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`) §I verification matrix enumerates the dimensions (component type, mutation, execution mode, compile failures, COM outcomes, artifact integrity, resolver/signatures, runtime, lifecycle/stress).
- M.1–M.3 have retained handoff notes with confirmed parity details; M.4 has an implementation/evidence note but remains manual/blocked by the CLI-created object-code defect and separate pre-existing escalations.

**Research questions delegated to this future TVS (slice-scoped only):**
- R-M.5.1: Are there parity gaps remaining after M.1–M.4 that the verification matrix surfaces? (This slice runs the consolidated matrix and catalogs any residual gaps; it does not fix them — it opens follow-up slices if needed.)
- R-M.5.2: Which Desktop runtime checks can be automated versus only manually observed, per component type? (Strategy executes; Indicator/ShowMe plot; TradingApp launches; Function exercised through a caller.)
- R-M.5.3: Are there documentation inconsistencies across `docs/SIGNATURE_CATALOG.md`, `docs/FEASIBILITY.md`, `docs/PROJECT_HISTORY.md`, `docs/E2E_TEST_SPEC.md`, `docs/TS_COMPONENT_TYPES.md` introduced or left stale by M.1–M.4?

**Implementation boundary:**
- In scope: consolidated E2E matrix execution, residual-gap catalog, documentation closure (correcting stale statements, adding M.1–M.4 outcomes), final runtime gate.
- Out of scope: new parity concerns beyond cataloging (open follow-up slices instead), root-cause changes, source-text parsing, password fiction.
- Must not retroactively edit history silently; per root-cause plan (`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`) §K ("Historical pre-L.3 signatures, resolver, tests, and documentation plan"), prepend dated corrections to historical docs rather than rewriting events.
- Must enforce the durable-path invariant from §5: any authoritative reference to the former `tmp/unknown_link_*` research docs in `docs/FEASIBILITY.md` or `docs/SIGNATURE_CATALOG.md` must use the durable `docs/RE/` paths (`RE_UNKNOWN_LINK_ROOT_CAUSE.md` / `RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`); M.5's closure review validates this and corrects any residual `tmp/unknown_link_*` reference.

**Likely files/systems:**
- `docs/E2E_TEST_SPEC.md` (extend with parity matrix if needed).
- `docs/SIGNATURE_CATALOG.md`, `docs/FEASIBILITY.md`, `docs/PROJECT_HISTORY.md`, `docs/TS_COMPONENT_TYPES.md` (closure edits).
- `tests/run_regression.ps1` and sub-suites (consolidated run).
- No production code changes in M.5. Any residual implementation gap pauses closure and is assigned to a separate reviewed slice; M.5 resumes only after that slice is complete.

**Verification closure sequence:**
1. Define and run the consolidated parity matrix (extending §I of the root-cause plan, `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`); catalog failures.
2. Complete R-M.5.1–R-M.5.3 analysis within this slice.
3. If any required cell needs production implementation, pause M.5 and open a separate reviewed slice. Do not patch it inside M.5.
4. After required follow-up slices are complete, rerun the matrix, close documentation gaps with dated corrections, and record final runtime results. M.5 is GREEN only when the required matrix passes with no unresolved blocking gap.

**Acceptance criteria:**
- Consolidated parity matrix passes for all five component types × all operation modes.
- Residual-gap catalog produced; every required implementation gap is resolved through a separate slice before closure, while explicitly optional gaps are documented with rationale.
- Documentation closed: no stale parity statements, dated corrections prepended where history is preserved.
- Final runtime gate: Strategy executes in Desktop without reverify; other types verified to the extent feasible (per R-M.5.2).
- No source-text parsing, no password fiction, no scope creep.

**Mandatory CLI E2E:**
- Full real CLI E2E per `docs/E2E_TEST_SPEC.md` for all five component types and all operation modes. This is the closure gate. Must pass before M.5 is considered done.

**Review/handoff requirements:**
- Adversarial review: confirm matrix completeness, documentation closure correctness, no silent history rewriting, no scope creep, complete execution.
- Handoff note recording residual gaps (if any) and follow-up slices opened.

---

## 3. Scope contamination prevention (hard rules for all M slices)

1. **Not the confirmed immediate root cause.** No M item may be presented, scoped, tested, or accepted as a root-cause fix. The root cause is Workstream L (STA COM + remove `FUN_10011230` bypass). Any conflation in a slice's tests, docs, or handoff is a DISAPPROVE finding.
2. **No source-text metadata parsing.** Per `AGENTS.md`, names/types/ProgIds come from orlib20 warehouse records or APIs. Any slice that appears to require source parsing is out of scope and must be re-scoped to a warehouse/API source. This applies explicitly to M.4 contract-change detection.
3. **No password-protection fiction.** Per the corrected findings in `docs/RE/RE_M2_SAVEMODULE_NEW.md` §6, there is no user password protection for library items. M.2 handles internal source encryption semantics only; it must not introduce any user-facing password concept, CLI flag, Python parameter, or error type.
4. **No bypass restoration.** No M item may restore, conditionally re-add, or emulate the removed `FUN_10011230` bypass, nor add compensating stack manipulation.
5. **No cross-slice bundling.** Each M.x is a separate TVS. One subagent → one narrowly scoped subtask (`AGENTS.md`). No multi-concern slices.
6. **No TradeStation installation modification.** Read-only per `AGENTS.md`.
7. **Do not reopen or hold L.** The current Workstream L status is stated in the document header. M retains its independent statuses and blockers above.

---

## 4. Safe recursion/cycle handling and failure reporting for dependent reverification (M.4-specific, planned not prescribed)

This section plans the safety requirements for M.4 without prescribing unsupported implementation details.

**Required safety properties (any M.4 implementation must satisfy these):**
- **Repeated-node/cycle safety:** The traversal must not hang, stack-overflow, or infinitely recurse when its enumerator encounters repeated nodes or a cycle. Prove this first with a synthetic test graph. Use a live warehouse cycle only if supported construction is confirmed. The guard may be TSDev's confirmed mechanism or an evidence-justified equivalent; do not assume a particular data structure before R-M.4.3.
- **Depth bound:** Match a confirmed TSDev bound when one exists. Otherwise establish a reviewed finite-safety invariant for the chosen mechanism; do not infer that a particular visited-set representation is mandatory.
- **Failure isolation:** A per-dependent reverify failure must not corrupt the root operation's durable result (L's ELL/XML pair) nor leave partial warehouse state. Failure reporting must include the dependent's name and ProgId (warehouse-derived, not source-derived).
- **Failure reporting shape:** Per-dependent failures must be surfaced in the CLI/Python result structure (consistent with existing JSON schemas and exit-code mapping) with enough identity detail for the user to locate the failing dependent. The exact shape is delegated to M.4's TDD plan and adversarial review.
- **No silent success:** If any dependent reverify fails, the operation must not report unqualified success. Either the operation fails with dependent detail, or it succeeds-with-warnings per TSDev's confirmed behavior (R-M.4.4). The choice must be cited from evidence.

**Not prescribed (delegated to M.4 research):**
- The exact visited-set data structure.
- The exact traversal order.
- Which supported orlib/warehouse primitives implement the TSDev.exe wrapper's behavior, and whether an equivalent loadable API exists (R-M.4.5). Direct calls into TSDev.exe are forbidden.
- The exact contract-change detection field (R-M.4.2).

---

## 5. Version/signature impact decision gates and documentation updates

Each M slice may touch signatures, the resolver, or the catalog. A decision gate applies before merge.

**Gate conditions (must be satisfied before merge of any M slice with resolver/catalog impact):**
- Classify every new target before editing metadata: named or ordinal PE exports use export-resolution metadata; non-exported internal functions use byte signatures. Do not represent an export as a byte signature merely to reuse existing machinery.
- If M.1 confirms ordinals 66/67 or another export pair, add the appropriate named/ordinal export resolver support, update the manifest export list and `EXPECTED_EXPORT_COUNT`, regenerate through the normal build, and update catalog/resolver tests. If the target is instead non-exported, use the signature path and update `EXPECTED_SIGNATURE_COUNT`.
- Apply the same classification to M.2 CELProperties targets and any additional **orlib20** primitives needed by M.4. Do not add a signature for TSDev.exe `FUN_004D91E0`; M.4 replicates its behavior without coupling the CLI to TSDev.exe.
- Update each changed expected count in its matching constant (`EXPECTED_SIGNATURE_COUNT`, `EXPECTED_EXPORT_COUNT`, or data-global count); never use the signature count for export changes and never hand-edit `signatures.gen.h`. Do not add dummy entries to preserve counts.
- Version detection (Tier 1/2/3) must remain unchanged: manifest identity, PE timestamp/file size, and SHA-256 are properties of the unmodified on-disk DLL. Run all mismatch tests after any resolver/index cleanup.

**Documentation updates required per slice:**
- M.1: update `docs/SIGNATURE_CATALOG.md` if ordinals 66/67 added; update `src/tsdev_lib.cpp` comments; slice-local findings note.
- M.2: update save-path comments; if encryption save-side parity confirmed, document the branch; slice-local findings note.
- M.3: document `0x40A` format/packing/lifetime in a durable note; update `docs/E2E_TEST_SPEC.md` if receiver-aware live check added.
- M.4: document traversal, contract-change detection, cycle handling, failure reporting in a durable note (likely `docs/RE/` or a slice-local findings note promoted to `docs/`).
- M.5: close documentation gaps across `docs/SIGNATURE_CATALOG.md`, `docs/FEASIBILITY.md`, `docs/PROJECT_HISTORY.md`, `docs/E2E_TEST_SPEC.md`, `docs/TS_COMPONENT_TYPES.md` with dated corrections (do not silently rewrite history).

**Stale-statement corrections (historical carry-forward; resolved):**
- The current correction notices and retired-target notes in
  `docs/SIGNATURE_CATALOG.md` and `docs/FEASIBILITY.md` record the removed
  persistence bypass, the current catalog/codegen counts, and the caller-cleanup
  ABI finding from `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md` §1. Their
  pre-L.3 bodies remain explicitly labeled historical/superseded. This is not
  an unresolved M.5 action; M.5 must not treat the historical text as current
  guidance.

**Durable-path references for former `tmp/unknown_link_*` research docs (current invariant + future validation):**
- The former `tmp/unknown_link_root_cause_and_plan.md` and `tmp/unknown_link_ghidra_evidence.md` research artifacts are now durably promoted to `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md` and `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`. **Current invariant:** any authoritative reference to that former research in `docs/FEASIBILITY.md` or `docs/SIGNATURE_CATALOG.md` must cite the durable `docs/RE/` paths, not the transient `tmp/unknown_link_*` paths. **Future validation:** M.5's documentation-closure review must verify this invariant holds in `docs/FEASIBILITY.md` and `docs/SIGNATURE_CATALOG.md` and correct any residual `tmp/unknown_link_*` reference to its durable `docs/RE/` path (coordinating with L.3 item 12, which owns the same invariant on the L side).

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
| M slice conflated with L root cause in tests/docs/handoff | Medium | High (scope contamination) | §3 hard rules; adversarial review explicitly checks for conflation; DISAPPROVE on any conflation. |
| M.4 cycle termination is not established when `contractChanged=true` by default | Medium | High (unsafe traversal; no claim that a hang definitely occurs) | Separate M.4 blocker; resolve old-record capture/gate behavior and execute synthetic/runtime cycle traversal coverage before acceptance. |
| M.2 reintroduces user password fiction | Low | High (violates corrected findings) | §3 hard rule; adversarial review; no user-facing password CLI/Python/error surface. |
| M.1 ordinal 66/67 unconfirmed or version-specific | Medium | Medium (parity gap) | R-M.1.1 must cite evidence; if unconfirmed, slice documents the gap and does not guess. |
| M.3 `0x40A` packing wrong → receivers ignore/misbehave | Medium | Medium (live refresh gap) | R-M.3.1 must cite evidence; receiver fixture mandatory; controlled observation of an actual confirmed consumer where feasible. |
| M.3b version=0 root cause not found or fix requires orlib20 modification | Medium | High (M.4 blocked) | R-M.3b.1–R-M.3b.7 must cite evidence; if the fix requires orlib20 modification (not allowed), document as a permanent limitation and adapt M.4 tests to use existing items only. |
| M.5 documentation closure silently rewrites history | Low | Medium (loss of audit trail) | §5 requires dated corrections prepended, not silent edits; adversarial review checks. |
| M slice expands scope into unrelated orlib20 subsystems | Medium | Medium (scope creep) | §3 hard rule; one subagent → one narrowly scoped subtask; adversarial review checks scope. |
| M.4 blocker overlooked → M.5 starts prematurely | Medium | High (invalid acceptance claim) | Header status and M.3b/M.4 blocker; no M.5 closure claim until the CLI-created object-code defect and M.4 dependency gate are resolved. |
| Signature/catalog count drift after M.1/M.2/M.4 | Medium | Medium (resolver/test failures) | §5 gate; regenerate via build, not hand-edit; run all mismatch tests. |
| Receiver-aware test (M.3) flaky due to window timing | Low | Low (test only) | Use a deterministic test-only window proc; do not rely on real TSDev for the unit gate. |

---

## 7. Atomic subagent orchestration and adversarial review requirements

**Orchestration (per `AGENTS.md`):**
- Each M.x is one TVS owned by the future PM, but no single subagent executes an entire TVS. Decompose it into atomic research, RED-test edit, RED verification, implementation edit, build, GREEN verification, regression, documentation, review, and real-CLI E2E subtasks as applicable.
- Each R-M.x.y question is a separate narrow subtask unless two questions are inseparable. Use the `universal` profile by default; it has full tool/MCP access. Fall back to `subagent_general` only if `universal` fails. Use `subagent_explore` only for trivial read-only lookups.
- If research reveals additional independent concerns, add reviewed subtasks or propose a new TVS; never expand one subagent's scope silently.
- Order: M.1 first (compiler setup informs M.4 and HWND questions). M.2 and M.3 may proceed in parallel after M.1 when they do not touch the same files. **M.3b follows M.1 (and possibly M.2 if save flags affect the version field) — it fixes the CLI object-code version=0 defect that blocks M.4.** M.4 follows M.3b (M.4's regression test creates CLI-compiled items which need proper element arrays). M.5 is last. The executing PM re-confirms dependencies and highest ROI before each slice.
- Each slice updates `docs/PM_WORKING_MEMORY.md` with its state (in-flight / approved / merged) only as part of that slice's own work.
- Any slice adding or registering tests updates CMake/runner wiring and all live current-count summaries (`AGENTS.md`, `README.md`, `docs/BUILD.md`, `docs/E2E_TEST_SPEC.md`) from actual collected definitions in that same slice; do not defer count accuracy to M.5.

**Adversarial review (per `AGENTS.md`):**
- Every M slice result (research findings note, implementation, documentation closure) must be adversarially reviewed by an independent subagent.
- The reviewer receives the original slice instructions (the relevant M.x section above) and the result. It checks for:
  - Complete execution of the slice instructions (objective 100% met).
  - No scope creep (no cross-slice bundling, no unrelated orlib20 subsystems).
  - No drift (no root-cause conflation, no presenting parity as the fix).
  - No undue shortcuts/hacks (no source-text parsing, no password fiction, no bypass restoration, no suppressed compiler errors).
  - TDD red→green actually followed (red test committed and shown failing before green).
  - Mandatory CLI E2E actually run and passed for all five component types in the relevant modes.
  - Documentation/signature gate (§5) satisfied if the slice touches signatures/resolver/catalog.
- On DISAPPROVE: an implementer subagent fixes the findings, then another adversarial reviewer reviews again. Loop until APPROVE.
- The closure slice (M.5) additionally adversarially reviews the consolidated matrix and documentation closure for completeness and no silent history rewriting.

**Handoff discipline:**
- Each slice produces a handoff note recording: confirmed research answers, implementation summary, test/E2E results, signature/catalog impact (if any), residual gaps, and follow-up slices opened (if any).
- Handoff notes are durable (promoted to `docs/` or `docs/RE/` where they have lasting value; kept in `tmp/` only if slice-local and not referenced by future slices).
